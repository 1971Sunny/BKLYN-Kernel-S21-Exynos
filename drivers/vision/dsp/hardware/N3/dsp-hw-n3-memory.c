// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
#include <linux/ion_exynos.h>
#include <linux/exynos_iovmm.h>
#include <asm/cacheflush.h>
#else
#include <linux/dma-buf.h>
#include <linux/ion.h>
#endif
#include <dt-bindings/soc/samsung/exynos-dsp.h>

#include "dsp-log.h"
#include "dsp-hw-n3-system.h"
#include "hardware/dummy/dsp-hw-dummy-memory.h"
#include "dsp-hw-n3-memory.h"

#if !defined(CONFIG_EXYNOS_IOVMM) || !defined(CONFIG_ION_EXYNOS)
static bool __dsp_hw_n3_memory_ion_cached_dmabuf(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer;

	dsp_check();
	buffer = dmabuf->priv;
	return !!(buffer->flags & ION_FLAG_CACHED);
}
#endif

static int dsp_hw_n3_memory_map_buffer(struct dsp_memory *mem,
		struct dsp_buffer *buf)
{
	int ret;
	struct dma_buf *dbuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t iova;

	dsp_enter();
	if (buf->fd <= 0) {
		ret = -EINVAL;
		dsp_err("fd(%d) is invalid\n", buf->fd);
		goto p_err;
	}

	dbuf = dma_buf_get(buf->fd);
	if (IS_ERR(dbuf)) {
		ret = PTR_ERR(dbuf);
		dsp_err("dma_buf is invalid(%d/%d)\n", buf->fd, ret);
		goto p_err;
	}
	buf->dbuf = dbuf;

	if (buf->size + buf->offset > dbuf->size) {
		ret = -EINVAL;
		dsp_err("size is invalid(%zu/%u/%zu)\n",
				buf->size, buf->offset, dbuf->size);
		goto p_err_size;
	}
	buf->dbuf_size = dbuf->size;

	attach = dma_buf_attach(dbuf, mem->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		dsp_err("Failed to attach dma_buf(%d)\n", ret);
		goto p_err_attach;
	}
	buf->attach = attach;

	sgt = dma_buf_map_attachment(attach, buf->dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		dsp_err("Failed to map attachment(%d)\n", ret);
		goto p_err_map_attach;
	}
	buf->sgt = sgt;

#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	iova = ion_iovmm_map(attach, 0, buf->dbuf_size, buf->dir, IOMMU_CACHE);
#else
	iova = sg_dma_address(buf->sgt->sgl);
#endif
	if (IS_ERR_VALUE(iova)) {
		ret = (int)iova;
		dsp_err("Failed to map iova(%d)\n", ret);
		goto p_err_map_dva;
	}
	buf->iova = iova;

	/* dma-coherent is supported. cached values used only for debugging */
#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	buf->cached = ion_cached_dmabuf(dbuf);
#else
	buf->cached = __dsp_hw_n3_memory_ion_cached_dmabuf(dbuf);
#endif

	dsp_dbg("buffer (%d/%zu/%#x/%d)\n",
			buf->fd, buf->dbuf_size, (int)iova, buf->cached);

	dsp_leave();
	return 0;
p_err_map_dva:
	dma_buf_unmap_attachment(attach, sgt, buf->dir);
p_err_map_attach:
	dma_buf_detach(dbuf, attach);
p_err_attach:
p_err_size:
	dma_buf_put(dbuf);
p_err:
	return ret;
}

static int dsp_hw_n3_memory_unmap_buffer(struct dsp_memory *mem,
		struct dsp_buffer *buf)
{
	dsp_enter();
#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	ion_iovmm_unmap(buf->attach, buf->iova);
#endif
	dma_buf_unmap_attachment(buf->attach, buf->sgt, buf->dir);
	dma_buf_detach(buf->dbuf, buf->attach);
	dma_buf_put(buf->dbuf);
	dsp_leave();
	return 0;
}

static int dsp_hw_n3_memory_sync_for_device(struct dsp_memory *mem,
		struct dsp_buffer *buf)
{
	dsp_enter();
	if (!buf->cached)
		return 0;

	if (!buf->kvaddr) {
		dsp_err("kvaddr is required to sync cacheable area(%d)\n",
				buf->fd);
		return -EINVAL;
	}

#if defined(CONFIG_EXYNOS_DSP_HW_N3)
	__dma_map_area(buf->kvaddr + buf->offset, buf->size, buf->dir);
#endif

	dsp_leave();
	return 0;
}

static int dsp_hw_n3_memory_sync_for_cpu(struct dsp_memory *mem,
		struct dsp_buffer *buf)
{
	dsp_enter();
	if (!buf->cached)
		return 0;

	if (!buf->kvaddr) {
		dsp_err("kvaddr is required to sync cacheable area(%d)\n",
				buf->fd);
		return -EINVAL;
	}

#if defined(CONFIG_EXYNOS_DSP_HW_N3)
	__dma_unmap_area(buf->kvaddr + buf->offset, buf->size, buf->dir);
#endif

	dsp_leave();
	return 0;
}

static int __dsp_hw_n3_memory_iovmm_map_sg(struct dsp_memory *mem,
		struct dsp_priv_mem *pmem)
{
	size_t size;

	dsp_enter();
	size = iommu_map_sg(mem->domain, pmem->iova, pmem->sgt->sgl,
			pmem->sgt->orig_nents, 0);
	if (!size) {
		dsp_err("Failed to map sg[%s]\n", pmem->name);
		return -ENOMEM;
	}

	if (size != pmem->size) {
		dsp_warn("pmem size(%zd) is different from mapped size(%zd)\n",
				pmem->size, size);
		pmem->size = size;
	}

	dsp_leave();
	return 0;
}

#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
extern void exynos_sysmmu_tlb_invalidate(struct iommu_domain *iommu_domain,
		dma_addr_t d_start, size_t size);
#endif

static int __dsp_hw_n3_memory_iovmm_unmap(struct dsp_memory *mem,
		struct dsp_priv_mem *pmem)
{
	size_t size;

	dsp_enter();
	size = iommu_unmap(mem->domain, pmem->iova, pmem->size);
	if (!size) {
		dsp_err("Failed to unmap sg[%s]\n", pmem->name);
		return -EINVAL;
	}
#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	exynos_sysmmu_tlb_invalidate(mem->domain, pmem->iova, pmem->size);
#endif

	dsp_leave();
	return 0;
}

static int dsp_hw_n3_memory_alloc(struct dsp_memory *mem,
		struct dsp_priv_mem *pmem)
{
	int ret;
	void *kvaddr;

	dsp_enter();
	kvaddr = vzalloc(pmem->size);
	if (!kvaddr) {
		ret = -ENOMEM;
		dsp_err("Failed to alloc kmem[%s]\n", pmem->name);
		goto p_err_alloc;
	}
	pmem->kvaddr = kvaddr;

	dsp_info("[%16s] %zuKB\n", pmem->name, pmem->size / SZ_1K);

	dsp_leave();
	return 0;
p_err_alloc:
	return ret;
}

static void dsp_hw_n3_memory_free(struct dsp_memory *mem,
		struct dsp_priv_mem *pmem)
{
	dsp_enter();
	vfree(pmem->kvaddr);
	dsp_leave();
}

#if !defined(CONFIG_EXYNOS_IOVMM) || !defined(CONFIG_ION_EXYNOS)
static unsigned int __dsp_hw_n3_get_heap_mask(void)
{
	unsigned int heap_mask = 0;
	int idx, cnt;
	struct ion_heap_data data[ION_NUM_MAX_HEAPS];
	const char *vheap_name = "vendor_system_heap";
	const char *sheap_name = "ion_system_heap";

	dsp_enter();
	cnt = ion_query_heaps_kernel(NULL, 0);
	ion_query_heaps_kernel((struct ion_heap_data *)data, cnt);

	for (idx = 0; idx < cnt; ++idx) {
		if (!strncmp(data[idx].name, vheap_name, MAX_HEAP_NAME)) {
			heap_mask = 1 << data[idx].heap_id;
			goto p_found;
		}
	}

	for (idx = 0; idx < cnt; ++idx) {
		if (!strncmp(data[idx].name, sheap_name, MAX_HEAP_NAME)) {
			heap_mask = 1 << data[idx].heap_id;
			goto p_found;
		}
	}

p_found:
	dsp_leave();
	return heap_mask;
}
#endif

static int dsp_hw_n3_memory_ion_alloc(struct dsp_memory *mem,
		struct dsp_priv_mem *pmem)
{
	int ret;
#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	const char *heap_name = "ion_system_heap";
#else
	unsigned int heap_mask;
#endif
	struct dma_buf *dbuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t iova;
	void *kvaddr;

	dsp_enter();
	/* unused memory */
	if (!pmem->size)
		return 0;

#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	dbuf = ion_alloc_dmabuf(heap_name, pmem->size, pmem->flags);
#else
	heap_mask = __dsp_hw_n3_get_heap_mask();
	if (!heap_mask) {
		ret = -EINVAL;
		dsp_err("Failed to get heap_mask(%d)\n", ret);
		goto p_err_heap_mask;
	}

	dbuf = ion_alloc(pmem->size, heap_mask, pmem->flags);
#endif
	if (IS_ERR(dbuf)) {
		ret = PTR_ERR(dbuf);
		dsp_err("Failed to allocate dma_buf(%d)[%s]\n",
				ret, pmem->name);
		goto p_err_alloc;
	}
	pmem->dbuf = dbuf;
	pmem->dbuf_size = dbuf->size;

	attach = dma_buf_attach(dbuf, mem->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		dsp_err("Failed to attach dma_buf(%d)[%s]\n",
				ret, pmem->name);
		goto p_err_attach;
	}
	pmem->attach = attach;

	sgt = dma_buf_map_attachment(attach, pmem->dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		dsp_err("Failed to map attachment(%d)[%s]\n",
				ret, pmem->name);
		goto p_err_map_attach;
	}
	pmem->sgt = sgt;

	if (pmem->kmap) {
		kvaddr = dma_buf_vmap(dbuf);
		if (!kvaddr) {
			ret = -EFAULT;
			dsp_err("Failed to map kvaddr(%d)[%s]\n",
					ret, pmem->name);
			goto p_err_kmap;
		}
		pmem->kvaddr = kvaddr;

		if (pmem->backup) {
			kvaddr = vzalloc(pmem->size);
			if (!kvaddr) {
				ret = -ENOMEM;
				dsp_err("Failed to alloc backup[%s]\n",
						pmem->name);
				goto p_err_backup;
			}
			pmem->bac_kvaddr = kvaddr;
		} else {
			pmem->bac_kvaddr = NULL;
		}
	} else {
		pmem->kvaddr = NULL;
	}

	if (pmem->fixed_iova) {
		ret = __dsp_hw_n3_memory_iovmm_map_sg(mem, pmem);
		if (ret)
			goto p_err_map_dva;
	} else {
#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
		iova = ion_iovmm_map(attach, 0, pmem->size, pmem->dir,
				IOMMU_CACHE);
#else
		iova = sg_dma_address(pmem->sgt->sgl);
#endif
		if (IS_ERR_VALUE(iova)) {
			ret = (int)iova;
			dsp_err("Failed to map iova(%d)[%s]\n",
					ret, pmem->name);
			goto p_err_map_dva;
		}
		pmem->iova = iova;
	}

	dsp_info("[%16s] %#x/%zuKB(%u)\n",
			pmem->name, (int)pmem->iova, pmem->size / SZ_1K,
			pmem->kmap);

	dsp_leave();
	return 0;
p_err_map_dva:
	if (pmem->kmap && pmem->backup)
		vfree(pmem->bac_kvaddr);
p_err_backup:
	if (pmem->kmap)
		dma_buf_vunmap(dbuf, pmem->kvaddr);
p_err_kmap:
	dma_buf_unmap_attachment(attach, sgt, pmem->dir);
p_err_map_attach:
	dma_buf_detach(dbuf, attach);
p_err_attach:
	dma_buf_put(dbuf);
p_err_alloc:
#if !defined(CONFIG_EXYNOS_IOVMM) || !defined(CONFIG_ION_EXYNOS)
p_err_heap_mask:
#endif
	return ret;
}

static void dsp_hw_n3_memory_ion_free(struct dsp_memory *mem,
		struct dsp_priv_mem *pmem)
{
	dsp_enter();
	/* unused memory */
	if (!pmem->size)
		return;

	if (pmem->fixed_iova)
		__dsp_hw_n3_memory_iovmm_unmap(mem, pmem);
#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	else
		ion_iovmm_unmap(pmem->attach, pmem->iova);
#endif

	if (pmem->kmap && pmem->backup)
		vfree(pmem->bac_kvaddr);

	if (pmem->kmap)
		dma_buf_vunmap(pmem->dbuf, pmem->kvaddr);

	dma_buf_unmap_attachment(pmem->attach, pmem->sgt, pmem->dir);
	dma_buf_detach(pmem->dbuf, pmem->attach);
	dma_buf_put(pmem->dbuf);
	dsp_leave();
}

static int __dsp_hw_n3_memory_priv_alloc(struct dsp_memory *mem)
{
	int ret;
	struct dsp_priv_mem *pmem;
	int idx;

	dsp_enter();
	pmem = mem->priv_mem;

	for (idx = 0; idx < mem->priv_mem_count; ++idx) {
		ret = mem->ops->ion_alloc(mem, &pmem[idx]);
		if (ret)
			goto p_err;
	}

	dsp_leave();
	return 0;
p_err:
	for (idx -= 1; idx >= 0; --idx)
		mem->ops->ion_free(mem, &pmem[idx]);

	return ret;
}

static void __dsp_hw_n3_memory_priv_free(struct dsp_memory *mem)
{
	struct dsp_priv_mem *pmem;
	int idx;

	dsp_enter();
	pmem = mem->priv_mem;

	for (idx = 0; idx < mem->priv_mem_count; ++idx)
		mem->ops->ion_free(mem, &pmem[idx]);

	dsp_leave();
}

static int dsp_hw_n3_memory_open(struct dsp_memory *mem)
{
	int ret;

	dsp_enter();
	ret = __dsp_hw_n3_memory_priv_alloc(mem);
	if (ret)
		goto p_err;

	dsp_leave();
	return 0;
p_err:
	return ret;
}

static int dsp_hw_n3_memory_close(struct dsp_memory *mem)
{
	dsp_enter();
	__dsp_hw_n3_memory_priv_free(mem);
	dsp_leave();
	return 0;
}

static int dsp_hw_n3_memory_probe(struct dsp_system *sys)
{
	int ret;
	struct dsp_memory *mem;
	struct dsp_priv_mem *pmem_list, *pmem;

	dsp_enter();
	mem = &sys->memory;
	mem->dev = sys->dev;
	mem->priv_mem_count = DSP_N3_PRIV_MEM_COUNT;

#if defined(CONFIG_EXYNOS_IOVMM) && defined(CONFIG_ION_EXYNOS)
	dma_set_mask(mem->dev, DMA_BIT_MASK(36));
	mem->domain = get_domain_from_dev(mem->dev);
#endif

	pmem_list = kcalloc(DSP_N3_PRIV_MEM_COUNT, sizeof(*pmem_list),
			GFP_KERNEL);
	if (!pmem_list) {
		ret = -ENOMEM;
		dsp_err("Failed to alloc dsp_priv_mem\n");
		goto p_err;
	}

	pmem = &pmem_list[DSP_N3_PRIV_MEM_FW_STACK];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "fw_stack");
	pmem->size = PAGE_ALIGN(DSP_N3_FW_STACK_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_FW_STACK_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_FW_STACK_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_TO_DEVICE;
	pmem->kmap = false;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_FW_STACK_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_FW];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "fw");
	pmem->size = PAGE_ALIGN(DSP_N3_FW_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_FW_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE
			- DSP_N3_FW_STACK_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_TO_DEVICE;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_FW_IOVA;
	pmem->backup = true;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_FW_LOG];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "fw_log");
	pmem->size = PAGE_ALIGN(DSP_N3_FW_LOG_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_FW_LOG_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_BIDIRECTIONAL;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_FW_LOG_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_IVP_PM];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "ivp_pm");
	pmem->size = PAGE_ALIGN(DSP_N3_IVP_PM_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_IVP_PM_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_TO_DEVICE;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_IVP_PM_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_IVP_DM];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "ivp_dm");
	pmem->size = PAGE_ALIGN(DSP_N3_IVP_DM_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_IVP_DM_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_TO_DEVICE;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_IVP_DM_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_IAC_PM];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "iac_pm");
	pmem->size = PAGE_ALIGN(DSP_N3_IAC_PM_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_IAC_PM_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_TO_DEVICE;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_IAC_PM_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_IAC_DM];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "iac_dm");
	pmem->size = PAGE_ALIGN(DSP_N3_IAC_DM_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_IAC_DM_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_TO_DEVICE;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_IAC_DM_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_MBOX_MEMORY];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "mbox_memory");
	pmem->size = PAGE_ALIGN(DSP_N3_MBOX_MEMORY_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_MBOX_MEMORY_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_BIDIRECTIONAL;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_MBOX_MEMORY_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_MBOX_POOL];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "mbox_pool");
	pmem->size = PAGE_ALIGN(DSP_N3_MBOX_POOL_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_MBOX_POOL_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_BIDIRECTIONAL;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_MBOX_POOL_IOVA;
	pmem->backup = false;

	pmem = &pmem_list[DSP_N3_PRIV_MEM_DL_OUT];
	snprintf(pmem->name, DSP_MEM_NAME_LEN, "dl_out");
	pmem->size = PAGE_ALIGN(DSP_N3_DL_OUT_SIZE);
	pmem->min_size = PAGE_ALIGN(DSP_N3_DL_OUT_SIZE);
	pmem->max_size = PAGE_ALIGN(DSP_N3_MEMORY_MAX_SIZE);
	pmem->flags = 0;
	pmem->dir = DMA_BIDIRECTIONAL;
	pmem->kmap = true;
	pmem->fixed_iova = true;
	pmem->iova = DSP_N3_DL_OUT_IOVA;
	pmem->backup = false;

	mem->priv_mem = pmem_list;
	mem->priv_mem_count = DSP_N3_PRIV_MEM_COUNT;
	mem->priv_ivp_pm_id = DSP_N3_PRIV_MEM_IVP_PM;
	mem->priv_dl_out_id = DSP_N3_PRIV_MEM_DL_OUT;

	dsp_leave();
	return 0;
p_err:
	return ret;
}

static void dsp_hw_n3_memory_remove(struct dsp_memory *mem)
{
	dsp_enter();
	kfree(mem->priv_mem);
	dsp_leave();
}

static const struct dsp_memory_ops n3_memory_ops = {
	.map_buffer		= dsp_hw_n3_memory_map_buffer,
	.unmap_buffer		= dsp_hw_n3_memory_unmap_buffer,
	.sync_for_device	= dsp_hw_n3_memory_sync_for_device,
	.sync_for_cpu		= dsp_hw_n3_memory_sync_for_cpu,

	.alloc			= dsp_hw_n3_memory_alloc,
	.free			= dsp_hw_n3_memory_free,
	.ion_alloc		= dsp_hw_n3_memory_ion_alloc,
	.ion_free		= dsp_hw_n3_memory_ion_free,
	.ion_alloc_secure	= dsp_hw_dummy_memory_ion_alloc_secure,
	.ion_free_secure	= dsp_hw_dummy_memory_ion_free_secure,

	.open			= dsp_hw_n3_memory_open,
	.close			= dsp_hw_n3_memory_close,
	.probe			= dsp_hw_n3_memory_probe,
	.remove			= dsp_hw_n3_memory_remove,
};

int dsp_hw_n3_memory_init(void)
{
	int ret;

	dsp_enter();
	ret = dsp_memory_register_ops(DSP_DEVICE_ID_N3, &n3_memory_ops);
	if (ret)
		goto p_err;

	dsp_leave();
	return 0;
p_err:
	return ret;
}
