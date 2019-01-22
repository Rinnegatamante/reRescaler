#include <libk/string.h>
#include <libk/stdio.h>
#include <libk/ctype.h>
#include <vitasdk.h>
#include <taihen.h>
#include "renderer.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define HOOKS_NUM 2 // Hooked functions num

static uint8_t current_hook;
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];

void *fb = NULL;
void *src_fb = NULL;
int dst_x, dst_y, src_w, src_h, src_p;

// Available modes 
enum {
	RESCALER_OFF
};

char *str_mode[] = {
	"No Rescaler"
};

int mode = RESCALER_OFF;

// Simplified generic hooking function
void hookFunction(uint32_t nid, const void *func){
	hooks[current_hook] = taiHookFunctionImport(&refs[current_hook],TAI_MAIN_MODULE,TAI_ANY_LIBRARY,nid,func);
	current_hook++;
}

int sceGxmDisplayQueueAddEntry_patched(SceGxmSyncObject *oldBuffer, SceGxmSyncObject *newBuffer, const void *callbackData) {
	
	// Performing a data transfer with sceGxmTransfer if a new framebuffer got allocated
	if (fb != NULL) {
		updateFramebuf(fb, 960, 544, 960);
		switch (mode){
		case RESCALER_OFF:
			sceGxmTransferCopy(
				src_w, src_h,
				0x00000000, 0x00000000, SCE_GXM_TRANSFER_COLORKEY_NONE,
				SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
				SCE_GXM_TRANSFER_LINEAR,
				src_fb, 0, 0, src_p,
				SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
				SCE_GXM_TRANSFER_LINEAR,
				fb, dst_x, dst_y, 960 * sizeof(uint32_t),
				NULL, SCE_GXM_TRANSFER_FRAGMENT_SYNC, NULL);
			break;
		default:
			break;
		}
		drawStringF(5, 20, "reRescaler: %ix%i -> 960x544 (%s)", src_w, src_h, str_mode[mode]);
	}
	
	return TAI_CONTINUE(int, refs[1], oldBuffer, newBuffer, callbackData);
}

int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
	
	// For 960x544 games, the plugin is useless
	if (pParam->width != 960) {
		
		// Discarding constantness of pParam
		SceDisplayFrameBuf *p = (SceDisplayFrameBuf*)pParam;
		
		// Setup code
		if (fb == NULL) {
			
			// Allocating our 960x544 framebuffer
			uint32_t fb_size = ALIGN(960 * 544 * 4, 0x40000);
			SceUID memblock = sceKernelAllocMemBlock("reRescaler fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, fb_size, NULL);
			sceKernelGetMemBlockBase(memblock, &fb);
			
			// Not enough CDRAM memory, plugin disabled
			if (fb == NULL) return TAI_CONTINUE(int, refs[0], pParam, sync);
			
			// Mapping framebuffer into sceGxm and saving constant values for later usage
			sceGxmMapMemory(fb, fb_size, SCE_GXM_MEMORY_ATTRIB_RW);
			src_w = p->width;
			src_h = p->height;
			dst_x = (960 - src_w) / 2;
			dst_y = (544 - src_h) / 2;
			src_p = p->pitch * sizeof(uint32_t);
			
		}
		
		// Patching pParam and grabbing current framebuffer data address
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