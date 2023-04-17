#pragma once

#include <vector>
#include <functional>

namespace RSDK
{

struct RenderVertex;
// GFXSurfaceHW MUST be defined by render device
struct GFXSurface;

// THESE CAN BE OVER WHAT'S ACTUALLY USED
// better safe than sorry
inline RenderVertex *AllocateVertexBuffer(uint32 count);
inline uint16 *AllocateIndexBuffer(uint32 count);
inline void AddQuadsToBuffer(uint16 *indexBuffer, uint32 count);

void SetupGFXSurface(GFXSurface *surface);
void RemoveGFXSurface(GFXSurface *surface);

void PrepareLayerTextures();
void PopulateTilesTexture();

struct Shader {
    void *internal;
    void Use();
    void SetUniformI(const char *, int32);
    void SetUniformI2(const char *name, int32 x, int32 y);
    void SetUniformF(const char *, float);
    void SetUniformF2(const char *name, float x, float y);
    void SetTexture(const char *, void *);
    virtual void SetArgs() = 0;
    virtual ~Shader() = default;    
};

// GEOMETRY SHADERS
extern struct RectShader : public Shader {
    void SetArgs();
} rectShader;
extern struct CircleShader : public Shader {
    float innerRadius;
    void SetArgs();
} circleShader;

extern struct SpriteShader : public Shader {
    uint16 palette[PALETTE_BANK_COUNT][PALETTE_BANK_SIZE];
    uint8 gfxLineBuffer[SCREEN_YSIZE];
    void SetArgs();
} spriteShader;
extern struct DevTextShader : public Shader {
    void SetArgs();
} devTextShader;

extern struct FBNoneShader : public Shader {
    void SetArgs();
} fbNoneShader;
extern struct FBBlendShader : public Shader {
    void SetArgs();
} fbBlendShader;
extern struct FBAlphaShader : public Shader {
    float alpha;
    void SetArgs();
} fbAlphaShader;
extern struct FBAddShader : public Shader {
    float intensity;
    void SetArgs();
} fbAddShader;
extern struct FBSubShader : public Shader {
    float intensity;
    void SetArgs();
} fbSubShader;
// it's recommended to handle this one differently and immediately render when it happens. complete the render later
// lookup tables are simply too big
extern struct FBTintShader : public Shader {
    uint16* lookupTable;
    void SetArgs();
} fbTintShader;
extern struct FBMaskedShader : public Shader {
    int32 color;
    void SetArgs();
} fbMaskedShader;
extern struct FBUnmaskedShader : public Shader {
    int32 color;
    void SetArgs();
} fbUnmaskedShader;

Shader* GetFBShader(int32 ink, float alpha);

struct RenderState {
    Shader *shader   = nullptr;
    Shader *fbShader = nullptr;

    GFXSurface *texture = nullptr;

    uint32 clip_X1;
    uint32 clip_Y1;
    uint32 clip_X2;
    uint32 clip_Y2;

    RenderVertex *vertexBuffer = nullptr;
    uint16 *indexBuffer        = nullptr;
    // uint32 vertexOffset = 0;
    uint32 vertexCount = 0;
    // uint32 indexOffset  = 0;
    uint32 indexCount = 0;

    // use for comparisons if you wanna opt
    uint32 shaderSize   = 0;
    uint32 fbShaderSize = 0;
};

class RenderStateQueue
{

private:
    std::vector<RenderState> _vector;
    uint32 i             = 0;
    RenderState _default = {};

public:
    uint32 size           = 0;
    using value_type      = RenderState;
    using reference       = RenderState &;
    using const_reference = const RenderState &;
    using size_type       = size_t;

    inline void push(const_reference value)
    {
        if (_vector.size() > size) {
            _vector[size] = value;
        }
        else {
            _vector.push_back(value);
        }
        size++;
    }

    inline reference pop() { return _vector[i++]; }

    inline void finish()
    {
        i    = 0;
        size = 0;
    }

    inline const bool empty() const { return i >= size; }

    inline reference front()
    {
        if (_vector.size() <= i)
            return _default;
        return _vector[i];
    };
    inline reference last()
    {
        if (size == 0)
            return _default;
        return _vector[size - 1];
    };

    inline reference operator[](uint32 index) { return _vector[index]; };
};

inline void PushCurrentState(int32 screen);

struct float3 {
    float x;
    float y;
    float z;
};

struct float2 {
    float x;
    float y;
};

struct RenderVertex {
    float3 pos;
    uint32 color;
    float2 tex;
};

inline void PlaceQuad(RenderVertex* vertBuffer, float2 pos1, float2 pos2, float2 uv1, float2 uv2, uint32 color) {
    vertBuffer[0] = { { pos1.x, pos1.y, 1.0 }, color, { uv1.x, uv1.y } };
    vertBuffer[1] = { { pos2.x, pos1.y, 1.0 }, color, { uv2.x, uv1.y } };
    vertBuffer[2] = { { pos1.x, pos2.y, 1.0 }, color, { uv1.x, uv2.y } };
    vertBuffer[3] = { { pos2.x, pos2.y, 1.0 }, color, { uv2.x, uv2.y } };
}

} // namespace RSDK
