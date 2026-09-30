#ifndef D3D_RENDER_H
#define D3D_RENDER_H
typedef void* D3DRender;
D3DRender d3dRender_create(void* hwnd, int width, int height, int format);
void d3dRender_destory(D3DRender _this);
int d3dRender_present(D3DRender _this, unsigned char* data[8], int linesize[8]);
#endif
