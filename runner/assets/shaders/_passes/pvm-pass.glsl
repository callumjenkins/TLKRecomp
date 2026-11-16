// A Sony PVM's Trinitron picture: beams that widen with brightness, an aperture grille, a little glow.
// Written for TLKRecomp. Public domain / CC0-1.0.

#pragma parameter BEAM_MIN "Beam height, dark" 0.45 0.2 1.0 0.05
#pragma parameter BEAM_MAX "Beam height, bright" 0.85 0.4 1.6 0.05
#pragma parameter MASK "Aperture grille" 0.35 0.0 1.0 0.05
#pragma parameter MASK_WIDTH "Grille triad, screen pixels" 3.0 2.0 6.0 1.0
#pragma parameter GLOW "Glow" 0.05 0.0 0.5 0.01
#pragma parameter SHARPNESS "Horizontal sharpness" 3.0 1.0 6.0 0.5
#pragma parameter BRIGHTNESS "Brightness" 1.45 0.8 2.5 0.05

#ifdef VERTEX
in vec2 VertexCoord;
in vec2 TexCoord;
out vec2 vTexCoord;

void main() {
    vTexCoord = TexCoord;
    gl_Position = vec4(VertexCoord * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
uniform sampler2D Texture;
uniform vec2 TextureSize;
in vec2 vTexCoord;
out vec4 FragColor;

#ifdef PARAMETER_UNIFORM
uniform float BEAM_MIN;
uniform float BEAM_MAX;
uniform float MASK;
uniform float MASK_WIDTH;
uniform float GLOW;
uniform float SHARPNESS;
uniform float BRIGHTNESS;
#else
#define BEAM_MIN 0.45
#define BEAM_MAX 0.85
#define MASK 0.35
#define MASK_WIDTH 3.0
#define GLOW 0.05
#define SHARPNESS 3.0
#define BRIGHTNESS 1.45
#endif

vec3 Linear(vec2 texel) {
    return pow(texture(Texture, (texel + 0.5) / TextureSize).rgb, vec3(2.4));
}

// One of the game's lines at x, blending into the next pixel only near the pixel's edge.
vec3 Line(float x, float line) {
    float i = floor(x - 0.5), f = x - 0.5 - i;
    f = clamp((f - 0.5) * SHARPNESS + 0.5, 0.0, 1.0);
    return mix(Linear(vec2(i, line)), Linear(vec2(i + 1.0, line)), f);
}

// d is the distance from the beam's centre in lines. A brighter channel has a wider beam, so bright lines close
// the gap and dark ones leave it black.
vec3 Beam(vec3 c, float d) {
    vec3 w = mix(vec3(BEAM_MIN), vec3(BEAM_MAX), sqrt(c));
    return c * exp(-4.5 * (d / w) * (d / w));
}

void main() {
    vec2 pos = vTexCoord * TextureSize;
    float y = pos.y - 0.5, line = floor(y), d = y - line;
    vec3 col = Beam(Line(pos.x, line), d) + Beam(Line(pos.x, line + 1.0), 1.0 - d);

    vec2 t = floor(pos);
    vec3 glow = Linear(t + vec2(-1.0, 0.0)) + Linear(t + vec2(1.0, 0.0))
              + Linear(t + vec2(0.0, -1.0)) + Linear(t + vec2(0.0, 1.0));
    col += GLOW * 0.25 * glow;

    // RGB stripes across the screen's own pixels, scaled so the grille keeps the average brightness.
    float s = floor(mod(gl_FragCoord.x, MASK_WIDTH) * 3.0 / MASK_WIDTH);
    vec3 stripe = vec3(s == 0.0, s == 1.0, s == 2.0);
    vec3 mask = mix(vec3(1.0 - MASK), vec3(1.0), stripe) / (1.0 - MASK * 2.0 / 3.0);

    FragColor = vec4(pow(clamp(col * mask * BRIGHTNESS, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
#endif
