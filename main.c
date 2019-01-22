#include <libk/string.h>
#include <libk/stdio.h>
#include <libk/ctype.h>
#include <vitasdk.h>
#include <taihen.h>

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define HOOKS_NUM 2 // Hooked functions num

static uint8_t current_hook;
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];

void *fb = NULL;
void *src_fb = NULL;
int dst_x, dst_y, src_w, src_h, src_p;

// Simplified generic hooking function
void hookFunction(uint32_t nid, const void *func){
	hooks[current_hook] = taiHookFunctionImport(&refs[current_hook],TAI_MAIN_MODULE,TAI_ANY_LIBRARY,nid,func);
	current_hook++;
}

int sceGxmDisplayQueueAddEntry_patched(SceGxmSyncObject *oldBuffer, SceGxmSyncObject *newBuffer, const void *callbackData) {
	if (fb != NULL) {	
		sceGxmTransferCopy(
			src_w, src_h,
			0x00000000, 0x00000000, SCE_GXM_TRANSFER_COLORKEY_NONE,
			SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
			SCE_GXM_TRANSFER_LINEAR,
			src_fb,
			0, 0, src_p,
			SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
			SCE_GXM_TRANSFER_LINEAR,
			fb,
			dst_x, dst_y, 960 * sizeof(uint32_t),
			NULL, SCE_GXM_TRANSFER_FRAGMENT_SYNC, NULL);
	}
	return TAI_CONTINUE(int, refs[1], oldBuffer, newBuffer, callbackData);
}

int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
	
	if (pParam->width != 960) {
		SceDisplayFrameBuf *p = (SceDisplayFrameBuf*)pParam;
		if (fb == NULL) { 
			uint32_t fb_size = ALIGN(960 * 544 * 4, 0x40000);
			SceUID memblock = sceKernelAllocMemBlock("reRescaler fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, fb_size, NULL);;
			sceKernelGetMemBlockBase(memblock, &fb);
			sceGxmMapMemory(fb, fb_size, SCE_GXM_MEMORY_ATTRIB_RW);
			src_w = p->width;
			src_h = p->height;
			dst_x = (960 - src_w) / 2;
			dst_y = (544 - src_h) / 2;
			src_p = p->pitch * sizeof(uint32_t);
		}
		src_fb = p->base;
		p->pitch = p->width = 960;
		p->height = 544;
		p->base = fb;
	}
	return TAI_CONTINUE(int, refs[0], pParam, sync);
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	
	// Hooking functions
	//hookFunction(0x207AF96B, sceGxmCreateRenderTarget_patched);
	hookFunction(0x7A410B64, sceDisplaySetFrameBuf_patched);
	hookFunction(0xEC5C26B5, sceGxmDisplayQueueAddEntry_patched);
	
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	
	// Freeing hooks
	while (current_hook-- > 0){
		taiHookRelease(hooks[current_hook], refs[current_hook]);
	}
	
	return SCE_KERNEL_STOP_SUCCESS;	
}