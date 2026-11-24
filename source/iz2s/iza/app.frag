#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 1, std430) readonly buffer Storage { uint words[]; } app;
const ivec2 extent = ivec2(1920, 1080);
const uint ui = 0x1000u / 4u;
const uint video = 0x800000u / 4u;

float sampleByte(uint index) {
    uint value = app.words[video + index / 4u];
    return float((value >> ((index % 4u) * 8u)) & 255u);
}

vec3 sampleVideo(ivec2 sourceSize, ivec2 src, uint format) {
    uint luma = uint(src.y * sourceSize.x + src.x);
    if (format == 0u) return unpackUnorm4x8(app.words[video + luma]).rgb;
    ivec2 chromaSize = (sourceSize + 1) / 2;
    uint plane = uint(chromaSize.x * chromaSize.y);
    uint chroma = uint(sourceSize.x * sourceSize.y + (src.y / 2) * chromaSize.x + src.x / 2);
    bool full = format == 2u || format == 4u;
    float y = (sampleByte(luma) - (full ? 0.0 : 16.0)) / (full ? 255.0 : 219.0);
    float cb = (sampleByte(chroma) - 128.0) / (full ? 255.0 : 224.0);
    float cr = (sampleByte(chroma + plane) - 128.0) / (full ? 255.0 : 224.0);
    vec4 coefficients = format >= 3u ? vec4(1.5748, 0.187324, 0.468124, 1.8556)
                                     : vec4(1.4020, 0.344136, 0.714136, 1.7720);
    return clamp(vec3(y + coefficients.x * cr, y - coefficients.y * cb - coefficients.z * cr,
                      y + coefficients.w * cb), 0.0, 1.0);
}

void main() {
    ivec2 at = clamp(ivec2(uv * vec2(extent)), ivec2(0), extent - 1);
    vec4 overlay = unpackUnorm4x8(app.words[ui + uint(at.y * extent.x + at.x)]);
    if (app.words[0] == 0u || overlay.a == 1.0) {
        color = vec4(overlay.rgb, 1.0);
        return;
    }
    vec3 base = app.words[1] == 0u ? vec3(25, 17, 23) / 255.0 : vec3(25, 20, 23) / 255.0;
    ivec2 sourceSize = ivec2(app.words[1], app.words[2]);
    ivec2 rectSize = ivec2(app.words[5], app.words[6]);
    ivec2 rel = at - ivec2(app.words[3], app.words[4]);
    if (sourceSize.x > 0 && all(greaterThanEqual(rel, ivec2(0))) && all(lessThan(rel, rectSize))) {
        ivec2 src = min(ivec2(vec2(rel) * vec2(sourceSize) / vec2(rectSize)), sourceSize - 1);
        base = sampleVideo(sourceSize, src, app.words[8]);
    }
    color = vec4(overlay.rgb + base * (1.0 - overlay.a), 1.0);
}
