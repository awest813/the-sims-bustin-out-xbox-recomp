#ifndef SIMS_NV2A_D3D11_BACKEND_H
#define SIMS_NV2A_D3D11_BACKEND_H

/* Register the native Direct3D 11 hardware render backend with xboxrecomp's
 * NV2A pushbuffer executor. No-op if RECOMP_SOFTWARE_GPU=1 is set.
 * Must be called before the title starts submitting GPU work. */
void sims_d3d11_backend_register(void);

#endif /* SIMS_NV2A_D3D11_BACKEND_H */
