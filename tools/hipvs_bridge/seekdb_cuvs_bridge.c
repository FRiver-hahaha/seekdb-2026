/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsCagraSearchParams_t sp;
  /* Reused device scratch. Query benchmarks issue many identical-size calls;
   * allocating/freeing three RMM buffers per call dominated small batches. */
  float*                 qd;
  unsigned*              nd;
  float*                 dd;
  size_t                 qcap;   /* floats in qd */
  size_t                 ncap;   /* elements in nd/dd */
  size_t                 base_itopk;
  long n, dim;
} sk_cuvs_index;

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) goto fail_idx;
  h->base_itopk = h->sp->itopk_size;
  {
    /* Optional build knobs for reproducible ablations; defaults stay at the
     * hipVS/CAGRA values used by the freeze baseline. */
    const char* e = getenv("SEEKDB_CUVS_INTERMEDIATE_DEGREE");
    if (e && *e) { unsigned long v = strtoul(e, NULL, 10); if (v > 0) h->ip->intermediate_graph_degree = (size_t)v; }
    e = getenv("SEEKDB_CUVS_GRAPH_DEGREE");
    if (e && *e) { unsigned long v = strtoul(e, NULL, 10); if (v > 0) h->ip->graph_degree = (size_t)v; }
    e = getenv("SEEKDB_CUVS_NN_DESCENT_NITER");
    if (e && *e) { unsigned long v = strtoul(e, NULL, 10); if (v > 0) h->ip->nn_descent_niter = (size_t)v; }
    e = getenv("SEEKDB_CUVS_BUILD_ALGO");
    if (e && *e) {
      if (strcmp(e, "nn_descent") == 0) h->ip->build_algo = NN_DESCENT;
      else if (strcmp(e, "auto") == 0) h->ip->build_algo = AUTO_SELECT;
      else h->ip->build_algo = IVF_PQ;
    }
  }
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_sp;
  h->n = n; h->dim = dim;
  return h;

fail_sp:  cuvsCagraSearchParamsDestroy(h->sp);
fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || nq <= 0 || topk <= 0) return 1;
  int rc = 0;
  const size_t qneed = (size_t)nq * (size_t)h->dim;
  const size_t nneed = (size_t)nq * (size_t)topk;

  if (qneed > h->qcap) {
    if (h->qd) { cuvsRMMFree(h->res, h->qd, sizeof(float) * h->qcap); h->qd = NULL; }
    h->qcap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->qd, sizeof(float) * qneed) != CUVS_SUCCESS) { rc = 10; goto done; }
    h->qcap = qneed;
  }
  if (nneed > h->ncap) {
    if (h->nd) { cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * h->ncap); h->nd = NULL; }
    if (h->dd) { cuvsRMMFree(h->res, h->dd, sizeof(float) * h->ncap); h->dd = NULL; }
    h->ncap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->nd, sizeof(unsigned) * nneed) != CUVS_SUCCESS) { rc = 11; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&h->dd, sizeof(float) * nneed) != CUVS_SUCCESS) {
      cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * nneed);
      h->nd = NULL;
      rc = 12;
      goto done;
    }
    h->ncap = nneed;
  }
  if (cudaMemcpy(h->qd, query, sizeof(float) * qneed, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  if (h->sp == NULL && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc = 14; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    {
      size_t required_itopk = (size_t)topk * 2;
      h->sp->itopk_size = h->base_itopk > required_itopk ? h->base_itopk : required_itopk;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, h->nd, sizeof(unsigned) * nneed, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, h->dd, sizeof(float) * nneed, cudaMemcpyDefault);
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->qd) cuvsRMMFree(h->res, h->qd, sizeof(float) * h->qcap);
  if (h->nd) cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * h->ncap);
  if (h->dd) cuvsRMMFree(h->res, h->dd, sizeof(float) * h->ncap);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
