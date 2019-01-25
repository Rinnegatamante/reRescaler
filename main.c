#include <libk/string.h>
#include <libk/stdio.h>
#include <libk/ctype.h>
#include <vitasdk.h>
#include <taihen.h>
#include "math_utils.h"
#include "renderer.h"

#include "shaders/texture2d_f.h"
#include "shaders/texture2d_v.h"
#include "shaders/lcd3x_f.h"
#include "shaders/lcd3x_v.h"
#include "shaders/sharp_bilinear_f.h"
#include "shaders/sharp_bilinear_v.h"
#include "shaders/sharp_bilinear_simple_f.h"
#include "shaders/sharp_bilinear_simple_v.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define HOOKS_NUM 4 // Hooked functions num
#define MODES_NUM 5 // Available rescaling modes

static uint8_t current_hook;
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];

void *fb = NULL;
void *src_fb = NULL;
int dst_x, dst_y, src_w, src_h, src_p;

static const SceGxmProgram *const gxm_program_texture2d_v = (SceGxmProgram*)&texture2d_v;
static const SceGxmProgram *const gxm_program_texture2d_f = (SceGxmProgram*)&texture2d_f;
SceGxmProgram *complex_vshaders[] = {
	(SceGxmProgram*)&sharp_bilinear_v,
	(SceGxmProgram*)&sharp_bilinear_simple_v,
	(SceGxmProgram*)&lcd3x_v
};
SceGxmProgram *complex_fshaders[] = {
	(SceGxmProgram*)&sharp_bilinear_f,
	(SceGxmProgram*)&sharp_bilinear_simple_f,
	(SceGxmProgram*)&lcd3x_f
};
const SceGxmProgramParameter *shader_params[6];

SceGxmVertexProgram *vertex_program_patched = NULL;
SceGxmFragmentProgram *fragment_program_patched = NULL;

static SceGxmRenderTarget *gxm_render_target;
static SceGxmColorSurface gxm_color_surface;
static SceGxmTexture gxm_texture;
static SceGxmContext *gxm_context;
static SceGxmShaderPatcher *gxm_shader_patcher;
static SceGxmShaderPatcherId fragment_id, vertex_id;
static const SceGxmProgramParameter *position;
static const SceGxmProgramParameter *texcoord;
static const SceGxmProgramParameter *wvp;

static matrix4x4 mvp;
static vector3f *vertices = NULL;
static vector4f *vertices_big = NULL;
static uint16_t *indices = NULL;
static vector2f *texcoords = NULL;
uint8_t *gpu_buffer = NULL;
void *vertex_buffer, *fragment_buffer;

static uint64_t tick = 0;
static uint64_t tick2 = 0;

vector2f *orig_res = NULL;
vector2f *target_res = NULL;

int renderer_ready = 0;
int bilinear = 0;

// Available modes 
enum {
	RESCALER_OFF,
	ORIGINAL,
	SHARP_BILINEAR,
	SHARP_BILINEAR_SIMPLE,
	LCD_3X
};

char *str_mode[MODES_NUM] = {
	"No Rescaler",
	"Original",
	"Sharp Bilinear",
	"Sharp Bilinear Simple",
	"LCD x3"
};

int mode = SHARP_BILINEAR_SIMPLE;

void releaseOldShader() {
	if (vertex_program_patched != NULL) {
		renderer_ready = 0;
		sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, fragment_program_patched);
		sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher, vertex_program_patched);
		sceGxmShaderPatcherForceUnregisterProgram(gxm_shader_patcher, fragment_id);
		sceGxmShaderPatcherForceUnregisterProgram(gxm_shader_patcher, vertex_id);
	}
}

void setupGenericAttribs() {
	if (gpu_buffer == NULL) {
		
		// Setting up default modelviewprojection matrix
		matrix4x4 projection, modelview;
		matrix4x4_identity(modelview);
		matrix4x4_init_orthographic(projection, 0, 960, 544, 0, -1, 1);
		matrix4x4_multiply(mvp, projection, modelview);
	
		// Allocating a generic buffer to use for our stuffs
		uint32_t gpu_buffer_size = 4 * 1024;
		SceUID memblock = sceKernelAllocMemBlock("reRescaler gpu buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, gpu_buffer_size, NULL);
		sceKernelGetMemBlockBase(memblock, (void*)&gpu_buffer);
		sceGxmMapMemory(gpu_buffer, 4 * 1024, SCE_GXM_MEMORY_ATTRIB_RW);
		vertices = (vector3f*)gpu_buffer;
		indices = (uint16_t*)(&gpu_buffer[sizeof(vector3f) * 4]);
		texcoords = (vector2f*)(&gpu_buffer[(sizeof(vector3f) + sizeof(uint16_t)) * 4]);
		vertices_big = (vector4f*)(&gpu_buffer[(sizeof(vector3f) + sizeof(uint16_t) + sizeof(vector2f)) * 4]);
		orig_res = (vector2f*)(&gpu_buffer[(sizeof(vector3f) + sizeof(uint16_t) + sizeof(vector2f) + sizeof(vector4f)) * 4]);
		target_res = (vector2f*)(&gpu_buffer[(sizeof(vector3f) + sizeof(uint16_t) + sizeof(vector2f) + sizeof(vector4f)) * 4 + sizeof(vector2f)]);
		
		orig_res[0].x = src_w * 1.0f;
		orig_res[1].y = src_h * 1.0f;
		target_res[0].x = 960.0f;
		target_res[1].y = 544.0f;
		
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
		vertices_big[0].x = 0.0f;
		vertices_big[0].y = 0.0f;
		vertices_big[0].z = 0.0f;
		vertices_big[0].w = 1.0f;
		vertices_big[1].x = 0.0f;
		vertices_big[1].y = 544.0f;
		vertices_big[1].z = 0.0f;
		vertices_big[1].w = 1.0f;
		vertices_big[2].x = 960.0f;
		vertices_big[2].y = 544.0f;
		vertices_big[2].z = 0.0f;
		vertices_big[2].w = 1.0f;
		vertices_big[3].x = 960.0f;
		vertices_big[3].y = 0.0f;
		vertices_big[3].z = 0.0f;
		vertices_big[3].w = 1.0f;
		
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
	}
	renderer_ready = 1;
}

void setupStandardShaders() {
	
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
	SceGxmVertexAttribute vertex_attribute[2];
	SceGxmVertexStream vertex_stream[2];
	vertex_attribute[0].streamIndex = 0;
	vertex_attribute[0].offset = 0;
	vertex_attribute[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	vertex_attribute[0].componentCount = 3;
	vertex_attribute[0].regIndex = sceGxmProgramParameterGetResourceIndex(position);
	vertex_attribute[1].streamIndex = 1;
	vertex_attribute[1].offset = 0;
	vertex_attribute[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	vertex_attribute[1].componentCount = 2;
	vertex_attribute[1].regIndex = sceGxmProgramParameterGetResourceIndex(texcoord);
	vertex_stream[0].stride = sizeof(vector3f);
	vertex_stream[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	vertex_stream[1].stride = sizeof(vector2f);
	vertex_stream[1].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	
	// Creating our shader programs
	sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher,
		vertex_id, vertex_attribute,
		2, vertex_stream, 2, &vertex_program_patched);

	sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher,
		fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, NULL, gxm_program_texture2d_v,
		&fragment_program_patched);
	
	 setupGenericAttribs();
}

void setupComplexShader(int i) {
	
	// Registering our shaders
	sceGxmShaderPatcherRegisterProgram(
		gxm_shader_patcher,
		complex_vshaders[i],
		&vertex_id);
	sceGxmShaderPatcherRegisterProgram(
		gxm_shader_patcher,
		complex_fshaders[i],
		&fragment_id);
		
	// Getting references to our vertex streams/uniforms
	position = sceGxmProgramFindParameterByName(complex_vshaders[i], "aPosition");
	texcoord = sceGxmProgramFindParameterByName(complex_vshaders[i], "aTexcoord");
	wvp = sceGxmProgramFindParameterByName(complex_vshaders[i], "wvp");
	
	// Setting up our vertex stream attributes
	SceGxmVertexAttribute vertex_attribute[2];
	SceGxmVertexStream vertex_stream[2];
	vertex_attribute[0].streamIndex = 0;
	vertex_attribute[0].offset = 0;
	vertex_attribute[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	vertex_attribute[0].componentCount = 4;
	vertex_attribute[0].regIndex = sceGxmProgramParameterGetResourceIndex(position);
	vertex_attribute[1].streamIndex = 1;
	vertex_attribute[1].offset = 0;
	vertex_attribute[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	vertex_attribute[1].componentCount = 2;
	vertex_attribute[1].regIndex = sceGxmProgramParameterGetResourceIndex(texcoord);
	vertex_stream[0].stride = sizeof(vector4f);
	vertex_stream[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	vertex_stream[1].stride = sizeof(vector2f);
	vertex_stream[1].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	
	shader_params[0] = sceGxmProgramFindParameterByName(complex_vshaders[i], "IN.video_size");
	shader_params[1] = sceGxmProgramFindParameterByName(complex_vshaders[i], "IN.texture_size");
	shader_params[2] = sceGxmProgramFindParameterByName(complex_vshaders[i], "IN.output_size");
	shader_params[3] = sceGxmProgramFindParameterByName(complex_fshaders[i], "IN2.video_size");
	shader_params[4] = sceGxmProgramFindParameterByName(complex_fshaders[i], "IN2.texture_size");
	shader_params[5] = sceGxmProgramFindParameterByName(complex_fshaders[i], "IN2.output_size");
	
	// Creating our shader programs
	sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher,
		vertex_id, vertex_attribute,
		2, vertex_stream, 2, &vertex_program_patched);

	sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher,
		fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, NULL, complex_vshaders[i],
		&fragment_program_patched);
	
	setupGenericAttribs();
}

// Simplified generic hooking function
void hookFunction(uint32_t nid, const void *func){
	hooks[current_hook] = taiHookFunctionImport(&refs[current_hook],TAI_MAIN_MODULE,TAI_ANY_LIBRARY,nid,func);
	current_hook++;
}

int sceGxmShaderPatcherCreate_patched(const SceGxmShaderPatcherParams *params, SceGxmShaderPatcher **shaderPatcher){
	int res =  TAI_CONTINUE(int, refs[3], params, shaderPatcher);
	
	// Grabbing a reference to used shader patcher
	gxm_shader_patcher = *shaderPatcher;
	
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
		
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_LTRIGGER) {
			if (tick == 0) tick = sceKernelGetProcessTimeWide();
			else if (sceKernelGetProcessTimeWide() - tick > 4000000) {
				tick = 0;
				mode = (mode + 1) % MODES_NUM;
				if (mode > RESCALER_OFF && gxm_render_target != NULL) {
				
					// Setting up required shaders
					releaseOldShader();
					if (mode <= ORIGINAL) setupStandardShaders();
					else setupComplexShader(mode - 2);
			
				}
			}
		} else tick = 0;
		if (pad.buttons & SCE_CTRL_RTRIGGER) {
			if (tick2 == 0) tick2 = sceKernelGetProcessTimeWide();
			else if (sceKernelGetProcessTimeWide() - tick2 > 4000000) {
				tick2 = 0;
				bilinear = (bilinear + 1) % 2;
			}
		} else tick2 = 0;
		
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
		case ORIGINAL:
			if (!renderer_ready) break;
			sceGxmTextureInitLinear(&gxm_texture, src_fb, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, src_w, src_h, 0);
			sceGxmTextureSetMagFilter(&gxm_texture, bilinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
			sceGxmTextureSetMinFilter(&gxm_texture, bilinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
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
			sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &vertex_buffer);
			sceGxmSetUniformDataF(vertex_buffer, wvp, 0, 16, (const float*)mvp);
			sceGxmSetFragmentTexture(gxm_context, 0, &gxm_texture);
			sceGxmSetVertexStream(gxm_context, 0, vertices);
			sceGxmSetVertexStream(gxm_context, 1, texcoords);
			sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, indices, 4);
			sceGxmEndScene(gxm_context, NULL, NULL);
			break;
		// Performing a rescale with standard sceGxm without a display queue and custom shaders
		case SHARP_BILINEAR:
		case SHARP_BILINEAR_SIMPLE:
		case LCD_3X:
			if (!renderer_ready) break;
			sceGxmTextureInitLinear(&gxm_texture, src_fb, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, src_w, src_h, 0);
			sceGxmTextureSetMagFilter(&gxm_texture, bilinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
			sceGxmTextureSetMinFilter(&gxm_texture, bilinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
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
			sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &vertex_buffer);
			sceGxmReserveFragmentDefaultUniformBuffer(gxm_context, &fragment_buffer);
			sceGxmSetUniformDataF(vertex_buffer, wvp, 0, 16, (const float*)mvp);
			if (shader_params[0]) sceGxmSetUniformDataF(vertex_buffer, shader_params[0], 0, 2, (const float*)orig_res);
			if (shader_params[1]) sceGxmSetUniformDataF(vertex_buffer, shader_params[1], 0, 2, (const float*)orig_res);
			if (shader_params[2]) sceGxmSetUniformDataF(vertex_buffer, shader_params[2], 0, 2, (const float*)target_res);
			if (shader_params[3]) sceGxmSetUniformDataF(fragment_buffer, shader_params[3], 0, 2, (const float*)orig_res);
			if (shader_params[4]) sceGxmSetUniformDataF(fragment_buffer, shader_params[4], 0, 2, (const float*)orig_res);
			if (shader_params[5]) sceGxmSetUniformDataF(fragment_buffer, shader_params[5], 0, 2, (const float*)target_res);
			sceGxmSetFragmentTexture(gxm_context, 0, &gxm_texture);
			sceGxmSetVertexStream(gxm_context, 0, vertices_big);
			sceGxmSetVertexStream(gxm_context, 1, texcoords);
			sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, indices, 4);
			sceGxmEndScene(gxm_context, NULL, NULL);
			break;
		default:
			break;
		}
		drawStringF(5, 40, "0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X", shader_params[0], shader_params[1], shader_params[2], shader_params[3], shader_params[4], shader_params[5]); 
		drawStringF(5, 20, "reRescaler: %ix%i -> 960x544 (%s - %s)", src_w, src_h, str_mode[mode], bilinear ? "Bilinear ON" : "Bilinear OFF");
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
			render_target_params.scenesPerFrame = 1;
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
			
			if (mode > RESCALER_OFF) {
				
				// Setting up required shaders
				if (mode <= ORIGINAL) setupStandardShaders();
				else setupComplexShader(mode - 2);

			}
			
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