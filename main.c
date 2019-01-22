#include <libk/string.h>
#include <libk/stdio.h>
#include <libk/ctype.h>
#include <vitasdk.h>
#include <taihen.h>
#include "renderer.h"
#include "texture2d_f.h"
#include "texture2d_v.h"
#include "math_utils.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define HOOKS_NUM 5 // Hooked functions num

static uint8_t current_hook;
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];

void *fb = NULL;
void *src_fb = NULL;
int dst_x, dst_y, src_w, src_h, src_p;

static const SceGxmProgram *const gxm_program_texture2d_v = (SceGxmProgram*)&texture2d_v;
static const SceGxmProgram *const gxm_program_texture2d_f = (SceGxmProgram*)&texture2d_f;

SceGxmVertexProgram *vertex_program_patched;
SceGxmFragmentProgram *fragment_program_patched;

static SceGxmRenderTarget *gxm_render_target;
static SceGxmColorSurface gxm_color_surface;
static SceGxmSyncObject *gxm_sync_object;
static SceGxmTexture gxm_texture;
static SceGxmContext *gxm_context;
static SceGxmShaderPatcher *gxm_shader_patcher;
static SceGxmShaderPatcherId fragment_id, vertex_id;
static const SceGxmProgramParameter *position;
static const SceGxmProgramParameter *texcoord;
static const SceGxmProgramParameter *wvp;

static matrix4x4 mvp;
static vector3f *vertices = NULL;
static uint16_t *indices = NULL;
static vector2f *texcoords = NULL;

// Available modes 
enum {
	RESCALER_OFF,
	RESCALE_NOFILTER,
	ORIGINAL
};

char *str_mode[] = {
	"No Rescaler",
	"No Bilinear",
	"Original",
};

int mode = RESCALE_NOFILTER;

// Simplified generic hooking function
void hookFunction(uint32_t nid, const void *func){
	hooks[current_hook] = taiHookFunctionImport(&refs[current_hook],TAI_MAIN_MODULE,TAI_ANY_LIBRARY,nid,func);
	current_hook++;
}

int sceGxmShaderPatcherCreate_patched(const SceGxmShaderPatcherParams *params, SceGxmShaderPatcher **shaderPatcher){
	int res =  TAI_CONTINUE(int, refs[3], params, shaderPatcher);
	
	// Grabbing a reference to used shader patcher
	gxm_shader_patcher = *shaderPatcher;
	
	// Registering our shaders
	sceGxmShaderPatcherRegisterProgram(
		gxm_shader_patcher,
		gxm_program_texture2d_v,
		&vertex_id);
	sceGxmShaderPatcherRegisterProgram(
		gxm_shader_patcher,
		gxm_program_texture2d_f,
		&fragment_id);
		
	// Getting references to our vertex streams/uniforms
	position = sceGxmProgramFindParameterByName(gxm_program_texture2d_v, "position");
	texcoord = sceGxmProgramFindParameterByName(gxm_program_texture2d_v, "texcoord");
	wvp = sceGxmProgramFindParameterByName(gxm_program_texture2d_v, "wvp");
	
	// Setting up our vertex stream attributes
	SceGxmVertexAttribute texture2d_vertex_attribute[2];
	SceGxmVertexStream texture2d_vertex_stream[2];
	texture2d_vertex_attribute[0].streamIndex = 0;
	texture2d_vertex_attribute[0].offset = 0;
	texture2d_vertex_attribute[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	texture2d_vertex_attribute[0].componentCount = 3;
	texture2d_vertex_attribute[0].regIndex = sceGxmProgramParameterGetResourceIndex(position);
	texture2d_vertex_attribute[1].streamIndex = 1;
	texture2d_vertex_attribute[1].offset = 0;
	texture2d_vertex_attribute[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	texture2d_vertex_attribute[1].componentCount = 2;
	texture2d_vertex_attribute[1].regIndex = sceGxmProgramParameterGetResourceIndex(texcoord);
	texture2d_vertex_stream[0].stride = sizeof(vector3f);
	texture2d_vertex_stream[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	texture2d_vertex_stream[1].stride = sizeof(vector2f);
	texture2d_vertex_stream[1].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

	// Creating our shader programs
	sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher,
		vertex_id, texture2d_vertex_attribute,
		2, texture2d_vertex_stream, 2, &vertex_program_patched);

	sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher,
		fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, NULL, NULL,
		&fragment_program_patched);
		
	// Setting up default modelviewprojection matrix
	matrix4x4 projection, modelview;
	matrix4x4_identity(modelview);
	matrix4x4_init_orthographic(projection, 0, 960, 544, 0, -1, 1);
	matrix4x4_multiply(mvp, projection, modelview);
	
	// Allocating a generic buffer to use for our stuffs
	uint8_t *gpu_buffer = NULL;
	uint32_t gpu_buffer_size = 4 * 1024;
	SceUID memblock = sceKernelAllocMemBlock("reRescaler gpu buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, gpu_buffer_size, NULL);
	sceKernelGetMemBlockBase(memblock, &gpu_buffer);
	sceGxmMapMemory(gpu_buffer, 4 * 1024, SCE_GXM_MEMORY_ATTRIB_RW);
	vertices = (vector3f*)gpu_buffer;
	indices = (uint16_t*)(&gpu_buffer[sizeof(vector3f) * 4]);
	texcoords = (vector2f*)(&gpu_buffer[(sizeof(vector3f) + sizeof(uint16_t)) * 4]);
	
	// Setting up default vertices
	vertices[0].x = 0.0f;
	vertices[0].y = 0.0f;
	vertices[0].z = 1.0f;
	vertices[1].x = 0.0f;
	vertices[1].y = 544.0f;
	vertices[1].z = 1.0f;
	vertices[2].x = 960.0f;
	vertices[2].y = 544.0f;
	vertices[2].z = 1.0f;
	vertices[3].x = 960.0f;
	vertices[3].y = 0.0f;
	vertices[3].z = 1.0f;
	
	// Setting up default indices
	int i;
	for (i=0;i<4;i++){
		indices[i] = i;
	}
	
	// Setting up default texcoords
	texcoords[0].x = 0.0f;
	texcoords[0].y = 0.0f;
	texcoords[1].x = 0.0f;
	texcoords[1].y = 1.0f;
	texcoords[2].x = 1.0f;
	texcoords[2].y = 1.0f;
	texcoords[3].x = 1.0f;
	texcoords[3].y = 0.0f;
	
	return res;
}

int sceGxmCreateContext_patched(const SceGxmContextParams *params, SceGxmContext **context) {
	int res = TAI_CONTINUE(int, refs[2], params, context);
	
	// Grabbing a reference to the created sceGxm context
	gxm_context = *context;
	
	return res;
}

int sceGxmDisplayQueueAddEntry_patched(SceGxmSyncObject *oldBuffer, SceGxmSyncObject *newBuffer, const void *callbackData) {
	
	if (fb != NULL) {
		updateFramebuf(fb, 960, 544, 960);
		switch (mode){
			
		// Performing a data transfer with sceGxmTransfer if a new framebuffer got allocated
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
			
		// Performing a rescale with standard sceGxm without a display queue
		case RESCALE_NOFILTER:
		case ORIGINAL:
			sceGxmTextureInitLinear(&gxm_texture, src_fb, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, src_w, src_h, 0);
			sceGxmTextureSetMagFilter(&gxm_texture, mode == ORIGINAL ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
			sceGxmTextureSetMinFilter(&gxm_texture, mode == ORIGINAL ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
			sceGxmSetFrontFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
			sceGxmSetBackFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
			sceGxmSetFrontPolygonMode(gxm_context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
			sceGxmSetBackPolygonMode(gxm_context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
			sceGxmSetTwoSidedEnable(gxm_context, SCE_GXM_TWO_SIDED_DISABLED);
			sceGxmSetCullMode(gxm_context, SCE_GXM_CULL_NONE);
			sceGxmSetFrontVisibilityTestEnable(gxm_context,SCE_GXM_VISIBILITY_TEST_DISABLED);
			sceGxmSetBackVisibilityTestEnable(gxm_context,SCE_GXM_VISIBILITY_TEST_DISABLED);
			sceGxmBeginScene(gxm_context, 0, gxm_render_target,
				NULL, NULL, NULL, &gxm_color_surface, NULL);
			sceGxmSetFrontStencilFunc(gxm_context,SCE_GXM_STENCIL_FUNC_ALWAYS,SCE_GXM_STENCIL_OP_KEEP,SCE_GXM_STENCIL_OP_KEEP,SCE_GXM_STENCIL_OP_KEEP,0,0);
			sceGxmSetBackStencilFunc(gxm_context,SCE_GXM_STENCIL_FUNC_ALWAYS,SCE_GXM_STENCIL_OP_KEEP,SCE_GXM_STENCIL_OP_KEEP,SCE_GXM_STENCIL_OP_KEEP,0,0);
			sceGxmSetFrontDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
			sceGxmSetBackDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
			sceGxmSetVertexProgram(gxm_context, vertex_program_patched);
			sceGxmSetFragmentProgram(gxm_context, fragment_program_patched);
			void* vertex_wvp_buffer;
			sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &vertex_wvp_buffer);
			sceGxmSetUniformDataF(vertex_wvp_buffer, wvp, 0, 16, (const float*)mvp);
			sceGxmSetFragmentTexture(gxm_context, 0, &gxm_texture);
			sceGxmSetVertexStream(gxm_context, 0, vertices);
			sceGxmSetVertexStream(gxm_context, 1, texcoords);
			sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, indices, 4);
			sceGxmEndScene(gxm_context, NULL, NULL);
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
			
			// Creating a render target for our patched framebuffer
			SceGxmRenderTargetParams render_target_params;
			memset(&render_target_params, 0, sizeof(SceGxmRenderTargetParams));
			render_target_params.flags = 0;
			render_target_params.width = 960;
			render_target_params.height = 544;
			render_target_params.scenesPerFrame = 8;
			render_target_params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
			render_target_params.multisampleLocations = 0;
			render_target_params.driverMemBlock = -1;
			sceGxmCreateRenderTarget(&render_target_params, &gxm_render_target);
			
			// Initializing a color surface for our framebuffer
			sceGxmColorSurfaceInit(&gxm_color_surface,
				SCE_GXM_COLOR_FORMAT_A8B8G8R8,
				SCE_GXM_COLOR_SURFACE_LINEAR,
				SCE_GXM_COLOR_SURFACE_SCALE_NONE,
				SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
				960, 544, 960, fb);
			
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
	hookFunction(0xE84CE5B4, sceGxmCreateContext_patched);
	hookFunction(0x05032658, sceGxmShaderPatcherCreate_patched);
	
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	
	// Freeing hooks
	while (current_hook-- > 0){
		taiHookRelease(hooks[current_hook], refs[current_hook]);
	}
	
	return SCE_KERNEL_STOP_SUCCESS;	
}