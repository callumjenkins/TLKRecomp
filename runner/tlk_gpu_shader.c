/* tlk_gpu_shader.c -- shader presets on SDL's GPU renderer, as tools/glslp_to_spirv.py compiled them. */
#include "tlk_gpu_shader.h"

#include <string.h>

#include "tlk_gpu_shaders.h"

enum { kMaxParams = 32, kMaxUniformBytes = 512 };

static SDL_GPUDevice *s_dev;
static SDL_GPUShader *s_shader;
static SDL_GPURenderState *s_state;
static const TlkGpuPreset *s_preset;
static float s_values[kMaxParams];
static Uint32 s_frame;

int TlkGpuShaderInit(SDL_Renderer *ren) {
    s_dev = kTlkGpuPresetCount ? SDL_GetGPURendererDevice(ren) : NULL;
    if (s_dev && !(SDL_GetGPUShaderFormats(s_dev) & SDL_GPU_SHADERFORMAT_SPIRV)) s_dev = NULL;
    return s_dev != NULL;
}

static void Release(void) {
    if (s_state) SDL_DestroyGPURenderState(s_state);
    if (s_shader) SDL_ReleaseGPUShader(s_dev, s_shader);
    s_state = NULL;
    s_shader = NULL;
    s_preset = NULL;
}

static const TlkGpuPreset *Find(const char *path) {
    const char *base = SDL_strrchr(path, '/'), *b2 = SDL_strrchr(path, '\\');
    base = b2 > base ? b2 : base;
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n > 6 && !SDL_strcasecmp(base + n - 6, ".glslp")) n -= 6;
    for (int i = 0; i < kTlkGpuPresetCount; i++)
        if (strlen(kTlkGpuPresets[i].name) == n && !SDL_strncasecmp(kTlkGpuPresets[i].name, base, n))
            return &kTlkGpuPresets[i];
    return NULL;
}

int TlkGpuShaderSelect(SDL_Renderer *ren, const char *path) {
    const TlkGpuPreset *p = path && path[0] ? Find(path) : NULL;
    if (p && p == s_preset) return 1;
    Release();
    if (!p) return !(path && path[0]);
    if (!s_dev || p->param_count > kMaxParams || p->uniform_size > kMaxUniformBytes) return 0;
    SDL_GPUShaderCreateInfo si = {
        .code_size = p->spirv_size, .code = p->spirv, .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = 1, .num_uniform_buffers = p->uniform_count ? 1 : 0 };
    s_shader = SDL_CreateGPUShader(s_dev, &si);
    SDL_GPURenderStateCreateInfo ci = { .fragment_shader = s_shader };
    s_state = s_shader ? SDL_CreateGPURenderState(ren, &ci) : NULL;
    if (!s_state) {
        SDL_Log("shader %s: %s", p->name, SDL_GetError());
        Release();
        return 0;
    }
    s_preset = p;
    for (int i = 0; i < p->param_count; i++) s_values[i] = p->params[i].def;
    return 1;
}

int TlkGpuShaderActive(void) { return s_state != NULL; }

int TlkGpuShaderFilterLinear(void) { return s_preset && s_preset->filter_linear; }

int TlkGpuShaderSetParam(const char *name, float value) {
    for (int i = 0; s_preset && i < s_preset->param_count; i++)
        if (!strcmp(s_preset->params[i].name, name)) {
            s_values[i] = SDL_clamp(value, s_preset->params[i].min, s_preset->params[i].max);
            return 1;
        }
    return 0;
}

static void UniformValue(const char *name, int in_w, int in_h, int out_w, int out_h, float v[4]) {
    if (!strcmp(name, "InputSize") || !strcmp(name, "TextureSize")) { v[0] = (float)in_w; v[1] = (float)in_h; }
    else if (!strcmp(name, "OutputSize")) { v[0] = (float)out_w; v[1] = (float)out_h; }
    else if (!strcmp(name, "FrameCount")) v[0] = (float)s_frame;
    else if (!strcmp(name, "FrameDirection")) v[0] = 1.0f;
    else
        for (int i = 0; i < s_preset->param_count; i++)
            if (!strcmp(s_preset->params[i].name, name)) v[0] = s_values[i];
}

void TlkGpuShaderBegin(SDL_Renderer *ren, int in_w, int in_h, int out_w, int out_h) {
    if (!s_state) return;
    s_frame++;
    if (s_preset->uniform_count) {
        Uint8 buf[kMaxUniformBytes] = { 0 };
        for (int i = 0; i < s_preset->uniform_count; i++) {
            const TlkGpuUniform *u = &s_preset->uniforms[i];
            float v[4] = { 0 };
            UniformValue(u->name, in_w, in_h, out_w, out_h, v);
            if (u->is_int) {
                Sint32 n = (Sint32)v[0];
                memcpy(buf + u->offset, &n, sizeof n);
            } else memcpy(buf + u->offset, v, u->floats * sizeof(float));
        }
        SDL_SetGPURenderStateFragmentUniforms(s_state, 0, buf, s_preset->uniform_size);
    }
    SDL_SetGPURenderState(ren, s_state);
}

void TlkGpuShaderQuit(void) {
    Release();
    s_dev = NULL;
}
