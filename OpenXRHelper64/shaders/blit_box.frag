#version 450

// Eye blit with a filtered downscale (OpenXRHelperDownsample=box). Same
// interface as the plain blit fragment shader: bounded source UV from the
// push constant, sRGB-encoded UNORM source decoded to linear, written to the
// sRGB swapchain (the hardware encodes). Each output pixel averages
// ceil(footprint) x ceil(footprint) bilinear taps spread over its source
// footprint, so every source pixel contributes; averaging is in linear
// light. At footprint <= 1 (no downscale) it is one tap, like the plain blit.
// Compile: glslangValidator -V blit_box.frag -o blit_box.frag.spv

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform PushConstants
{
    vec4 bounds;
} pc;

vec3 srgbToLinear(vec3 c)
{
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, cutoff);
}

void main()
{
    vec2 uv = mix(pc.bounds.xy, pc.bounds.zw, clamp(vUv, vec2(0.0), vec2(1.0)));
    vec2 texSize = vec2(textureSize(srcTex, 0));
    // Source texels covered by one output pixel along each axis.
    vec2 footprint = max(vec2(abs(dFdx(uv.x)), abs(dFdy(uv.y))) * texSize, vec2(1.0));
    ivec2 taps = clamp(ivec2(ceil(footprint)), ivec2(1), ivec2(8));
    vec2 footprintUv = footprint / texSize;
    vec2 stepUv = footprintUv / vec2(taps);
    vec2 start = uv - 0.5 * footprintUv + 0.5 * stepUv;

    vec3 sum = vec3(0.0);
    float alpha = 0.0;
    for (int y = 0; y < taps.y; ++y)
    {
        for (int x = 0; x < taps.x; ++x)
        {
            vec4 c = texture(srcTex, start + vec2(float(x), float(y)) * stepUv);
            sum += srgbToLinear(clamp(c.rgb, vec3(0.0), vec3(1.0)));
            alpha += c.a;
        }
    }
    float n = float(taps.x * taps.y);
    outColor = vec4(sum / n, alpha / n);
}
