#include "GLFWRenderDevice.hpp"
#include "Rendering.hpp"
#ifndef _GLVERSION
#if EXTRA_HW_RENDER
#define _GLVERSION 32
#else
#define _GLVERSION 20
#endif
#endif

#if _GLVERSION >= 30
#define _GLSLVERSION "#version 130\n#define in_V in\n#define in_F in\n"
#else
#define _GLSLVERSION "#version 110\n#define in_V attribute\n#define out varying\n#define in_F varying\n"
#endif

#if RETRO_REV02
#define _GLDEFINE "#define RETRO_REV02 (1)\n"
#else
#define _GLDEFINE "\n"
#endif

const GLchar *backupVertex = R"aa(
in_V vec3 in_pos;
in_V vec2 in_UV;
out vec4 ex_color;
out vec2 ex_UV;

void main()
{
    gl_Position = vec4(in_pos, 1.0);
    ex_color    = vec4(1.0);
    ex_UV       = in_UV;
}
)aa";

const GLchar *backupFragment = R"aa(
in_F vec2 ex_UV;
in_F vec4 ex_color;

uniform sampler2D texDiffuse;

void main()
{
    gl_FragColor = texture2D(texDiffuse, ex_UV);
}
)aa";

GLFWwindow *RSDK::RenderDevice::window;
GLuint RSDK::RenderDevice::VAO;
GLuint RSDK::RenderDevice::VBO;

GLuint RSDK::RenderDevice::screenTextures[SCREEN_COUNT];
GLuint RSDK::RenderDevice::imageTexture;

double RSDK::RenderDevice::lastFrame;
double RSDK::RenderDevice::targetFreq;

int32 RSDK::RenderDevice::monitorIndex;

uint32 *RSDK::RenderDevice::videoBuffer;

#if EXTRA_HW_RENDER
float2 scaling;
float2 texPreScale;
#endif

bool RSDK::RenderDevice::Init()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, _GLVERSION / 10);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, _GLVERSION % 10);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    if (!videoSettings.bordered)
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    GLFWmonitor *monitor = NULL;
    int32 w, h;

    if (videoSettings.windowed) {
        w = videoSettings.windowWidth;
        h = videoSettings.windowHeight;
    }
    else if (videoSettings.fsWidth <= 0 || videoSettings.fsHeight <= 0) {
        monitor                 = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        w                       = mode->width;
        h                       = mode->height;
    }
    else {
        monitor = glfwGetPrimaryMonitor();
        w       = videoSettings.fsWidth;
        h       = videoSettings.fsHeight;
    }

    window = glfwCreateWindow(w, h, gameVerInfo.gameTitle, monitor, NULL);
    if (!window) {
        PrintLog(PRINT_NORMAL, "ERROR: [GLFW] window creation failed");
        return false;
    }
    PrintLog(PRINT_NORMAL, "w: %d h: %d windowed: %d", w, h, videoSettings.windowed);

    glfwSetKeyCallback(window, ProcessKeyEvent);
    glfwSetJoystickCallback(ProcessJoystickEvent);
    glfwSetMouseButtonCallback(window, ProcessMouseEvent);
    glfwSetWindowFocusCallback(window, ProcessFocusEvent);
    glfwSetWindowMaximizeCallback(window, ProcessMaximizeEvent);

    if (!SetupRendering() || !AudioDevice::Init())
        return false;

    InitInputDevices();
    return true;
}

bool RSDK::RenderDevice::SetupRendering()
{
    glfwMakeContextCurrent(window);
    GLenum err;
    if ((err = glewInit()) != GLEW_OK) {
        PrintLog(PRINT_NORMAL, "ERROR: failed to initialize GLEW: %s", glewGetErrorString(err));
        return false;
    }

    GetDisplays();

    if (!InitGraphicsAPI() || !InitShaders())
        return false;

    int32 size = videoSettings.pixWidth >= SCREEN_YSIZE ? videoSettings.pixWidth : SCREEN_YSIZE;
    scanlines  = (ScanlineInfo *)malloc(size * sizeof(ScanlineInfo));
    memset(scanlines, 0, size * sizeof(ScanlineInfo));

    videoSettings.windowState = WINDOWSTATE_ACTIVE;
    videoSettings.dimMax      = 1.0;
    videoSettings.dimPercent  = 1.0;

    return true;
}

void RSDK::RenderDevice::GetDisplays()
{
    GLFWmonitor *monitor = glfwGetWindowMonitor(window);
    if (!monitor)
        monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *displayMode = glfwGetVideoMode(monitor);
    int32 monitorCount;
    GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);

    for (int32 m = 0; m < monitorCount; ++m) {
        const GLFWvidmode *vidMode = glfwGetVideoMode(monitors[m]);
        displayWidth[m]            = vidMode->width;
        displayHeight[m]           = vidMode->height;
        if (!memcmp(vidMode, displayMode, sizeof(GLFWvidmode))) {
            monitorIndex = m;
        }
    }

    const GLFWvidmode *displayModes = glfwGetVideoModes(monitor, &displayCount);
    if (displayInfo.displays)
        free(displayInfo.displays);

    displayInfo.displays          = (decltype(displayInfo.displays))malloc(sizeof(GLFWvidmode) * displayCount);
    int32 newDisplayCount         = 0;
    bool32 foundFullScreenDisplay = false;

    for (int32 d = 0; d < displayCount; ++d) {
        memcpy(&displayInfo.displays[newDisplayCount].internal, &displayModes[d], sizeof(GLFWvidmode));

        int32 refreshRate = displayInfo.displays[newDisplayCount].refresh_rate;
        if (refreshRate >= 59 && (refreshRate <= 60 || refreshRate >= 120) && displayInfo.displays[newDisplayCount].height >= (SCREEN_YSIZE * 2)) {
            if (d && refreshRate == 60 && displayInfo.displays[newDisplayCount - 1].refresh_rate == 59)
                --newDisplayCount;

            if (videoSettings.fsWidth == displayInfo.displays[newDisplayCount].width
                && videoSettings.fsHeight == displayInfo.displays[newDisplayCount].height)
                foundFullScreenDisplay = true;

            ++newDisplayCount;
        }
    }

    displayCount = newDisplayCount;
    if (!foundFullScreenDisplay) {
        videoSettings.fsWidth     = 0;
        videoSettings.fsHeight    = 0;
        videoSettings.refreshRate = 60; // 0;
    }
}

#if EXTRA_HW_RENDER
bool SetupHWRendering();
void PrepareHWPass();
#endif

Vector2 viewportPos{}, viewportSize{};

bool RSDK::RenderDevice::InitGraphicsAPI()
{
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);

    // setup buffers

#if _GLVERSION >= 30
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);
#endif

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(RenderVertex) * (!RETRO_REV02 ? 24 : 60), NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), 0);
    glEnableVertexAttribArray(0);
    // glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex), (void *)offsetof(RenderVertex, color));
    // glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), (void *)offsetof(RenderVertex, tex));
    glEnableVertexAttribArray(1);

    if (videoSettings.windowed || !videoSettings.exclusiveFS) {
        if (videoSettings.windowed) {
            viewSize.x = videoSettings.windowWidth;
            viewSize.y = videoSettings.windowHeight;
        }
        else {
            viewSize.x = displayWidth[monitorIndex];
            viewSize.y = displayHeight[monitorIndex];
        }
    }
    else {
        int32 bufferWidth  = videoSettings.fsWidth;
        int32 bufferHeight = videoSettings.fsHeight;
        if (videoSettings.fsWidth <= 0 || videoSettings.fsHeight <= 0) {
            bufferWidth  = displayWidth[monitorIndex];
            bufferHeight = displayHeight[monitorIndex];
        }
        viewSize.x = bufferWidth;
        viewSize.y = bufferHeight;
    }

    int32 maxPixHeight = 0;
#if !RETRO_USE_ORIGINAL_CODE
    int32 screenWidth = 0;
#endif
    for (int32 s = 0; s < SCREEN_COUNT; ++s) {
        if (videoSettings.pixHeight > maxPixHeight)
            maxPixHeight = videoSettings.pixHeight;

        screens[s].size.y = videoSettings.pixHeight;

        float viewAspect = viewSize.x / viewSize.y;
#if !RETRO_USE_ORIGINAL_CODE
        screenWidth = (int32)((viewAspect * videoSettings.pixHeight) + 3) & 0xFFFFFFFC;
#else
        int32 screenWidth = (int32)((viewAspect * videoSettings.pixHeight) + 3) & 0xFFFFFFFC;
#endif
        if (screenWidth < videoSettings.pixWidth)
            screenWidth = videoSettings.pixWidth;

#if !RETRO_USE_ORIGINAL_CODE
        if (customSettings.maxPixWidth && screenWidth > customSettings.maxPixWidth)
            screenWidth = customSettings.maxPixWidth;
#else
        if (screenWidth > DEFAULT_PIXWIDTH)
            screenWidth = DEFAULT_PIXWIDTH;
#endif

        memset(&screens[s].frameBuffer, 0, sizeof(screens[s].frameBuffer));
        SetScreenSize(s, screenWidth, screens[s].size.y);
    }

    pixelSize.x     = screens[0].size.x;
    pixelSize.y     = screens[0].size.y;
    float pixAspect = pixelSize.x / pixelSize.y;

    Vector2 lastViewSize;

    glfwGetWindowSize(window, &lastViewSize.x, &lastViewSize.y);
    viewportSize = lastViewSize;

    if ((viewSize.x / viewSize.y) <= ((pixelSize.x / pixelSize.y) + 0.1)) {
        if ((pixAspect - 0.1) > (viewSize.x / viewSize.y)) {
            viewSize.y     = (pixelSize.y / pixelSize.x) * viewSize.x;
            viewportPos.y  = (lastViewSize.y >> 1) - (viewSize.y * 0.5);
            viewportSize.y = viewSize.y;
        }
    }
    else {
        viewSize.x     = pixAspect * viewSize.y;
        viewportPos.x  = (lastViewSize.x >> 1) - ((pixAspect * viewSize.y) * 0.5);
        viewportSize.x = (pixAspect * viewSize.y);
    }

#if !RETRO_USE_ORIGINAL_CODE
    if (screenWidth <= 512 && maxPixHeight <= 256) {
#else
    if (maxPixHeight <= 256) {
#endif
        textureSize.x = 512.0;
        textureSize.y = 256.0;
    }
    else {
        textureSize.x = 1024.0;
        textureSize.y = 512.0;
    }

#if EXTRA_HW_RENDER
    texPreScale = textureSize;
    scaling.x   = viewSize.x / pixelSize.x;
    scaling.y   = viewSize.y / pixelSize.y;

    while ((textureSize.x < scaling.x * screenWidth) || (textureSize.y < scaling.y * maxPixHeight)) {
        textureSize.x *= 2;
        textureSize.y *= 2;
    }
#endif

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(SCREEN_COUNT, screenTextures);

    for (int32 i = 0; i < SCREEN_COUNT; ++i) {
        glBindTexture(GL_TEXTURE_2D, screenTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, textureSize.x, textureSize.y, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);

        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glGenTextures(1, &imageTexture);
    glBindTexture(GL_TEXTURE_2D, imageTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, RETRO_VIDEO_TEXTURE_W, RETRO_VIDEO_TEXTURE_H, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    videoBuffer = new uint32[RETRO_VIDEO_TEXTURE_W * RETRO_VIDEO_TEXTURE_H];

    lastShaderID = -1;
    InitVertexBuffer();
    engine.inFocus          = 1;
    videoSettings.viewportX = viewportPos.x;
    videoSettings.viewportY = viewportPos.y;
    videoSettings.viewportW = 1.0 / viewSize.x;
    videoSettings.viewportH = 1.0 / viewSize.y;

#if EXTRA_HW_RENDER
    return SetupHWRendering();
#else
    return true;
#endif
}

// CUSTOM BUFFER FOR SHENANIGAN PURPOSES
// GL hates us and it's coordinate system is reverse of DX
// for shader output equivalency, we havee to flip everything
// X and Y are negated, some verts are specifically moved to match
// U and V are 0/1s and flipped from what it was originally
// clang-format off
#if RETRO_REV02
const RenderVertex rsdkGLVertexBuffer[60] = {
    // 1 Screen (0)
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    
    // 2 Screens - Bordered (Top Screen) (6)
    { { +0.5,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +0.5, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -0.5, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +0.5,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -0.5,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -0.5, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    
    // 2 Screens - Bordered (Bottom Screen) (12)
    { { +0.5, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +0.5,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -0.5,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +0.5, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -0.5, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -0.5,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    
    // 2 Screens - Stretched (Top Screen) (18)
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
  
    // 2 Screens - Stretched (Bottom Screen) (24)
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    
    // 4 Screens (Top-Left) (30)
    { {  0.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { {  0.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { {  0.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },

    // 4 Screens (Top-Right) (36)
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { {  0.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { {  0.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { {  0.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    
    // 4 Screens (Bottom-Right) (42)
    { {  0.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { {  0.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { {  0.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    
    // 4 Screens (Bottom-Left) (48)
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { {  0.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { {  0.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { {  0.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    
    // Image/Video (54)
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } }
};
#else
const RenderVertex rsdkGLVertexBuffer[24] =
{
    // 1 Screen (0)
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },

    // 2 Screens - Stretched (Top Screen) (6)
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
  
    // 2 Screens - Stretched (Bottom Screen) (12)
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0,  0.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
  
    // Image/Video (18)
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { +1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  0.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } },
    { { +1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  1.0,  1.0 } },
    { { -1.0, -1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  1.0 } },
    { { -1.0, +1.0,  1.0 }, 0xFFFFFFFF, {  0.0,  0.0 } }
};
#endif
// clang-format on

void RSDK::RenderDevice::InitVertexBuffer()
{
    RenderVertex vertBuffer[sizeof(rsdkGLVertexBuffer) / sizeof(RenderVertex)];
    memcpy(vertBuffer, rsdkGLVertexBuffer, sizeof(rsdkGLVertexBuffer));

    float x = 0.5 / (float)viewSize.x;
    float y = 0.5 / (float)viewSize.y;

    // ignore the last 6 verts, they're scaled to the 1024x512 textures already!
    int32 vertCount = (RETRO_REV02 ? 60 : 24) - 6;
    for (int32 v = 0; v < vertCount; ++v) {
        RenderVertex *vertex = &vertBuffer[v];
        vertex->pos.x        = vertex->pos.x + x;
        vertex->pos.y        = vertex->pos.y - y;

        if (vertex->tex.x)
            vertex->tex.x = screens[0].size.x * (1.0 / textureSize.x);

        if (vertex->tex.y)
            vertex->tex.y = screens[0].size.y * (1.0 / textureSize.y);
#if EXTRA_HW_RENDER
        if (vertex->tex.x)
            vertex->tex.x *= scaling.x;

        if (vertex->tex.y)
            vertex->tex.y *= scaling.y;
#endif
    }

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(RenderVertex) * (!RETRO_REV02 ? 24 : 60), vertBuffer);
}

void RSDK::RenderDevice::InitFPSCap()
{
    lastFrame  = glfwGetTime();
    targetFreq = 1.0 / videoSettings.refreshRate;
}
bool RSDK::RenderDevice::CheckFPSCap()
{
    if (lastFrame + targetFreq < glfwGetTime())
        return true;

    return false;
}
void RSDK::RenderDevice::UpdateFPSCap() { lastFrame = glfwGetTime(); }

#if !EXTRA_HW_RENDER
void RSDK::RenderDevice::CopyFrameBuffer()
{
    for (int32 s = 0; s < videoSettings.screenCount; ++s) {
        glBindTexture(GL_TEXTURE_2D, screenTextures[s]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, screens[s].pitch, SCREEN_YSIZE, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, screens[s].frameBuffer);
    }
}
#endif

bool RSDK::RenderDevice::ProcessEvents()
{
    glfwPollEvents();
    if (glfwWindowShouldClose(window))
        isRunning = false;
    return false;
}

void RSDK::RenderDevice::FlipScreen()
{
    if (lastShaderID != videoSettings.shaderID) {
        lastShaderID = videoSettings.shaderID;

        SetLinear(shaderList[videoSettings.shaderID].linear);

        if (videoSettings.shaderSupport)
            glUseProgram(shaderList[videoSettings.shaderID].programID);
    }

    if (windowRefreshDelay > 0) {
        windowRefreshDelay--;
        if (!windowRefreshDelay)
            UpdateGameWindow();
        return;
    }

#if EXTRA_HW_RENDER
    glBindVertexArray(VAO);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    SetLinear(shaderList[videoSettings.shaderID].linear);
    if (videoSettings.shaderSupport)
        glUseProgram(shaderList[videoSettings.shaderID].programID);
    glViewport(viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y);
    glDisable(GL_SCISSOR_TEST);
#endif

    glClear(GL_COLOR_BUFFER_BIT);
    if (videoSettings.shaderSupport) {
        glUniform2fv(glGetUniformLocation(shaderList[videoSettings.shaderID].programID, "textureSize"), 1, &texPreScale.x);
        glUniform2fv(glGetUniformLocation(shaderList[videoSettings.shaderID].programID, "pixelSize"), 1, &pixelSize.x);
        glUniform2fv(glGetUniformLocation(shaderList[videoSettings.shaderID].programID, "viewSize"), 1, &viewSize.x);
        glUniform1f(glGetUniformLocation(shaderList[videoSettings.shaderID].programID, "screenDim"), videoSettings.dimMax * videoSettings.dimPercent);
    }

    int32 startVert = 0;
    switch (videoSettings.screenCount) {
        default:
        case 0:
#if RETRO_REV02
            startVert = 54;
#else
            startVert = 18;
#endif
            glBindTexture(GL_TEXTURE_2D, imageTexture);
            glDrawArrays(GL_TRIANGLES, startVert, 6);

            break;

        case 1:
            glBindTexture(GL_TEXTURE_2D, screenTextures[0]);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            break;

        case 2:
#if RETRO_REV02
            startVert = startVertex_2P[0];
#else
            startVert = 6;
#endif
            glBindTexture(GL_TEXTURE_2D, screenTextures[0]);
            glDrawArrays(GL_TRIANGLES, startVert, 6);

#if RETRO_REV02
            startVert = startVertex_2P[1];
#else
            startVert = 12;
#endif
            glBindTexture(GL_TEXTURE_2D, screenTextures[1]);
            glDrawArrays(GL_TRIANGLES, startVert, 6);
            break;

#if RETRO_REV02
        case 3:
            // also flipped
            glBindTexture(GL_TEXTURE_2D, screenTextures[0]);
            glDrawArrays(GL_TRIANGLES, startVertex_3P[0], 6);

            glBindTexture(GL_TEXTURE_2D, screenTextures[1]);
            glDrawArrays(GL_TRIANGLES, startVertex_3P[1], 6);

            glBindTexture(GL_TEXTURE_2D, screenTextures[2]);
            glDrawArrays(GL_TRIANGLES, startVertex_3P[2], 6);
            break;

        case 4:
            // this too
            glBindTexture(GL_TEXTURE_2D, screenTextures[0]);
            glDrawArrays(GL_TRIANGLES, 30, 6);
            glBindTexture(GL_TEXTURE_2D, screenTextures[1]);
            glDrawArrays(GL_TRIANGLES, 36, 6);

            glBindTexture(GL_TEXTURE_2D, screenTextures[2]);
            glDrawArrays(GL_TRIANGLES, 42, 6);

            glBindTexture(GL_TEXTURE_2D, screenTextures[3]);
            glDrawArrays(GL_TRIANGLES, 48, 6);
            break;
#endif
    }

    glFlush();
    glfwSwapBuffers(window);
#if EXTRA_HW_RENDER
    PrepareHWPass();
#endif
}

void RSDK::RenderDevice::Release(bool32 isRefresh)
{
    glDeleteTextures(SCREEN_COUNT, screenTextures);
    glDeleteTextures(1, &imageTexture);
    if (videoBuffer)
        delete[] videoBuffer;
    for (int32 i = 0; i < shaderCount; ++i) {
        glDeleteProgram(shaderList[i].programID);
    }

#if _GLVERSION >= 30
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
#endif

    shaderCount = 0;
#if RETRO_USE_MOD_LOADER
    userShaderCount = 0;
#endif

    glfwDestroyWindow(window);

    if (!isRefresh) {
        if (displayInfo.displays)
            free(displayInfo.displays);
        displayInfo.displays = NULL;

        if (scanlines)
            free(scanlines);
        scanlines = NULL;

        glfwTerminate();
    }
}

bool RSDK::RenderDevice::InitShaders()
{
    videoSettings.shaderSupport = true;
    int32 maxShaders            = 0;
#if RETRO_USE_MOD_LOADER
    shaderCount = 0;
#endif

    LoadShader("None", false);
    LoadShader("Clean", true);
    LoadShader("CRT-Yeetron", true);
    LoadShader("CRT-Yee64", true);

#if RETRO_USE_MOD_LOADER
    // a place for mods to load custom shaders
    RunModCallbacks(MODCB_ONSHADERLOAD, NULL);
    userShaderCount = shaderCount;
#endif

    LoadShader("YUV-420", true);
    LoadShader("YUV-422", true);
    LoadShader("YUV-444", true);
    LoadShader("RGB-Image", true);
    maxShaders = shaderCount;

    // no shaders == no support
    if (!maxShaders) {
        ShaderEntry *shader         = &shaderList[0];
        videoSettings.shaderSupport = false;

        // let's load
        maxShaders  = 1;
        shaderCount = 1;

        GLint success;
        char infoLog[0x1000];

        GLuint vert, frag;
        const GLchar *vchar[] = { _GLSLVERSION, _GLDEFINE, backupVertex };
        vert                  = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vert, 3, vchar, NULL);
        glCompileShader(vert);

        const GLchar *fchar[] = { _GLSLVERSION, _GLDEFINE, backupFragment };
        frag                  = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(frag, 3, fchar, NULL);
        glCompileShader(frag);

        glGetShaderiv(vert, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(vert, 0x1000, NULL, infoLog);
            PrintLog(PRINT_NORMAL, "BACKUP vertex shader compiling failed:\n%s", infoLog);
        }

        glGetShaderiv(frag, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(frag, 0x1000, NULL, infoLog);
            PrintLog(PRINT_NORMAL, "BACKUP fragment shader compiling failed:\n%s", infoLog);
        }
        shader->programID = glCreateProgram();
        glAttachShader(shader->programID, vert);
        glAttachShader(shader->programID, frag);

        glBindAttribLocation(shader->programID, 0, "in_pos");
        // glBindAttribLocation(shader->programID, 1, "in_color");
        glBindAttribLocation(shader->programID, 1, "in_UV");

        glLinkProgram(shader->programID);
        glDeleteShader(vert);
        glDeleteShader(frag);

        glUseProgram(shader->programID);

        shader->linear = videoSettings.windowed ? false : shader->linear;
    }

    videoSettings.shaderID = MAX(videoSettings.shaderID >= maxShaders ? 0 : videoSettings.shaderID, 0);
    SetLinear(shaderList[videoSettings.shaderID].linear || videoSettings.screenCount > 1);

    return true;
}

GLuint GL_LoadShader(const char *, const char *, bool builtin = false);
GLuint GL_LoadShader(const char *vertf, const char *fragf, bool builtin)
{
    FileInfo info;
    GLint success;
    char infoLog[0x1000];
    GLuint vert, frag;
    InitFileInfo(&info);
    if (LoadFile(&info, vertf, FMODE_RB)) {
        uint8 *fileData = NULL;
        AllocateStorage((void **)&fileData, info.fileSize + 1, DATASET_TMP, false);
        ReadBytes(&info, fileData, info.fileSize);
        fileData[info.fileSize] = 0;
        CloseFile(&info);

        const GLchar *glchar[] = { _GLSLVERSION, _GLDEFINE, (const GLchar *)fileData };
        vert                   = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vert, 3, glchar, NULL);
        glCompileShader(vert);

        glGetShaderiv(vert, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(vert, 0x1000, NULL, infoLog);
            PrintLog(PRINT_NORMAL, "Vertex shader compiling failed:\n%s", infoLog);
            return 0;
        }
    }
    else
        return 0;

    InitFileInfo(&info);
    if (LoadFile(&info, fragf, FMODE_RB)) {
        uint8 *fileData = NULL;
        AllocateStorage((void **)&fileData, info.fileSize + 1, DATASET_TMP, false);
        ReadBytes(&info, fileData, info.fileSize);
        fileData[info.fileSize] = 0;
        CloseFile(&info);

        const GLchar *glchar[] = { _GLSLVERSION, _GLDEFINE, (const GLchar *)fileData };
        frag                   = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(frag, 3, glchar, NULL);
        glCompileShader(frag);

        glGetShaderiv(frag, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(frag, 0x1000, NULL, infoLog);
            PrintLog(PRINT_NORMAL, "Fragment shader compiling failed:\n%s", infoLog);
            return 0;
        }
    }
    else
        return 0;

    GLuint ret = glCreateProgram();
    glAttachShader(ret, vert);
    glAttachShader(ret, frag);

    glBindAttribLocation(ret, 0, "in_pos");
    if (!builtin)
        glBindAttribLocation(ret, 1, "in_color");
    glBindAttribLocation(ret, builtin ? 1 : 2, "in_UV");

    glLinkProgram(ret);
    glGetProgramiv(ret, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(ret, 0x1000, NULL, infoLog);
        PrintLog(PRINT_NORMAL, "OpenGL shader linking failed:\n%s", infoLog);
        return 0;
    }
    glDeleteShader(vert);
    glDeleteShader(frag);

    return ret;
}

void RSDK::RenderDevice::LoadShader(const char *fileName, bool32 linear)
{
    char fullFilePath[0x100];

    for (int32 i = 0; i < shaderCount; ++i) {
        if (strcmp(shaderList[i].name, fileName) == 0)
            return;
    }

    if (shaderCount == SHADER_COUNT)
        return;

    ShaderEntry *shader = &shaderList[shaderCount];
    shader->linear      = linear;
    sprintf_s(shader->name, sizeof(shader->name), "%s", fileName);

    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Shaders/OGL/%s.fs", fileName);
    shader->programID = GL_LoadShader("Data/Shaders/OGL/None.vs", fullFilePath, true);

    shaderCount++;
};

void RSDK::RenderDevice::RefreshWindow()
{
    videoSettings.windowState = WINDOWSTATE_UNINITIALIZED;

    Release(true);
    if (!videoSettings.bordered)
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    GLFWmonitor *monitor = NULL;
    int32 w, h;

    if (videoSettings.windowed) {
        w = videoSettings.windowWidth;
        h = videoSettings.windowHeight;
    }
    else if (videoSettings.fsWidth <= 0 || videoSettings.fsHeight <= 0) {
        monitor                 = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        w                       = mode->width;
        h                       = mode->height;
    }
    else {
        monitor = glfwGetPrimaryMonitor();
        w       = videoSettings.fsWidth;
        h       = videoSettings.fsHeight;
    }

    window = glfwCreateWindow(w, h, gameVerInfo.gameTitle, monitor, NULL);
    if (!window) {
        PrintLog(PRINT_NORMAL, "ERROR: [GLFW] window creation failed");
        return;
    }
    PrintLog(PRINT_NORMAL, "w: %d h: %d windowed: %d", w, h, videoSettings.windowed);

    glfwSetKeyCallback(window, ProcessKeyEvent);
    glfwSetMouseButtonCallback(window, ProcessMouseEvent);
    glfwSetWindowFocusCallback(window, ProcessFocusEvent);
    glfwSetWindowMaximizeCallback(window, ProcessMaximizeEvent);

    glfwMakeContextCurrent(window);

    if (!InitGraphicsAPI() || !InitShaders())
        return;

    videoSettings.windowState = WINDOWSTATE_ACTIVE;
}

void RSDK::RenderDevice::GetWindowSize(int32 *width, int32 *height)
{
    int32 widest = 0, highest = 0, count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    for (int32 i = 0; i < count; i++) {
        const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
        if (mode->height > highest) {
            highest = mode->height;
            widest  = mode->width;
        }
    }
    if (width)
        *width = widest;
    if (height)
        *height = highest;
}

void RSDK::RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels)
{
    if (imagePixels) {
        glBindTexture(GL_TEXTURE_2D, imageTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, imagePixels);
    }
}

void RSDK::RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY,
                                                  int32 strideU, int32 strideV)
{
    uint32 *pixels = videoBuffer;
    uint32 *preY   = pixels;
    int32 pitch    = RETRO_VIDEO_TEXTURE_W - width;

    if (videoSettings.shaderSupport) {
        for (int32 y = 0; y < height; ++y) {
            for (int32 x = 0; x < width; ++x) {
                *pixels++ = (yPlane[x] << 16) | 0xFF000000;
            }

            pixels += pitch;
            yPlane += strideY;
        }

        pixels = preY;
        pitch  = RETRO_VIDEO_TEXTURE_W - (width >> 1);
        for (int32 y = 0; y < (height >> 1); ++y) {
            for (int32 x = 0; x < (width >> 1); ++x) {
                *pixels++ |= (vPlane[x] << 0) | (uPlane[x] << 8) | 0xFF000000;
            }

            pixels += pitch;
            uPlane += strideU;
            vPlane += strideV;
        }
    }
    else {
        // No shader support means no YUV support! at least use the brightness to show it in grayscale!
        for (int32 y = 0; y < height; ++y) {
            for (int32 x = 0; x < width; ++x) {
                int32 brightness = yPlane[x];
                *pixels++        = (brightness << 0) | (brightness << 8) | (brightness << 16) | 0xFF000000;
            }

            pixels += pitch;
            yPlane += strideY;
        }
    }
    glBindTexture(GL_TEXTURE_2D, imageTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, RETRO_VIDEO_TEXTURE_W, RETRO_VIDEO_TEXTURE_H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, videoBuffer);
}

void RSDK::RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY,
                                                  int32 strideU, int32 strideV)
{
    uint32 *pixels = videoBuffer;
    uint32 *preY   = pixels;
    int32 pitch    = RETRO_VIDEO_TEXTURE_W - width;

    if (videoSettings.shaderSupport) {
        for (int32 y = 0; y < height; ++y) {
            for (int32 x = 0; x < width; ++x) {
                *pixels++ = (yPlane[x] << 16) | 0xFF000000;
            }

            pixels += pitch;
            yPlane += strideY;
        }

        pixels = preY;
        pitch  = RETRO_VIDEO_TEXTURE_W - (width >> 1);
        for (int32 y = 0; y < height; ++y) {
            for (int32 x = 0; x < (width >> 1); ++x) {
                *pixels++ |= (vPlane[x] << 0) | (uPlane[x] << 8) | 0xFF000000;
            }

            pixels += pitch;
            uPlane += strideU;
            vPlane += strideV;
        }
    }
    else {
        // No shader support means no YUV support! at least use the brightness to show it in grayscale!
        for (int32 y = 0; y < height; ++y) {
            for (int32 x = 0; x < width; ++x) {
                int32 brightness = yPlane[x];
                *pixels++        = (brightness << 0) | (brightness << 8) | (brightness << 16) | 0xFF000000;
            }

            pixels += pitch;
            yPlane += strideY;
        }
    }

    glBindTexture(GL_TEXTURE_2D, imageTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, RETRO_VIDEO_TEXTURE_W, RETRO_VIDEO_TEXTURE_H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, videoBuffer);
}
void RSDK::RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY,
                                                  int32 strideU, int32 strideV)
{
    uint32 *pixels = videoBuffer;
    int32 pitch    = RETRO_VIDEO_TEXTURE_W - width;

    if (videoSettings.shaderSupport) {
        for (int32 y = 0; y < height; ++y) {
            int32 pos1  = yPlane - vPlane;
            int32 pos2  = uPlane - vPlane;
            uint8 *pixV = vPlane;
            for (int32 x = 0; x < width; ++x) {
                *pixels++ = pixV[0] | (pixV[pos2] << 8) | (pixV[pos1] << 16) | 0xFF000000;
                pixV++;
            }

            pixels += pitch;
            yPlane += strideY;
            uPlane += strideU;
            vPlane += strideV;
        }
    }
    else {
        // No shader support means no YUV support! at least use the brightness to show it in grayscale!
        for (int32 y = 0; y < height; ++y) {
            for (int32 x = 0; x < width; ++x) {
                int32 brightness = yPlane[x];
                *pixels++        = (brightness << 0) | (brightness << 8) | (brightness << 16) | 0xFF000000;
            }

            pixels += pitch;
            yPlane += strideY;
        }
    }
    glBindTexture(GL_TEXTURE_2D, imageTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, RETRO_VIDEO_TEXTURE_W, RETRO_VIDEO_TEXTURE_H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, videoBuffer);
}

void RSDK::RenderDevice::ProcessKeyEvent(GLFWwindow *, int32 key, int32 scancode, int32 action, int32 mods)
{
    switch (action) {
        case GLFW_PRESS: {
#if !RETRO_REV02
            ++RSDK::SKU::buttonDownCount;
#endif
            switch (key) {
                case GLFW_KEY_ENTER:
                    if (mods & GLFW_MOD_ALT) {
                        videoSettings.windowed ^= 1;
                        UpdateGameWindow();
                        changedVideoSettings = false;
                        break;
                    }

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[1] = true;
#endif

                    // [fallthrough]

                default:
#if RETRO_INPUTDEVICE_KEYBOARD
                    SKU::UpdateKeyState(key);
#endif
                    break;

                case GLFW_KEY_ESCAPE:
                    if (engine.devMenu) {
#if RETRO_REV0U
                        if (sceneInfo.state == ENGINESTATE_DEVMENU || RSDK::Legacy::gameMode == RSDK::Legacy::ENGINE_DEVMENU)
#else
                        if (sceneInfo.state == ENGINESTATE_DEVMENU)
#endif
                            CloseDevMenu();
                        else
                            OpenDevMenu();
                    }
                    else {
#if RETRO_INPUTDEVICE_KEYBOARD
                        SKU::UpdateKeyState(key);
#endif
                    }

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[0] = true;
#endif
                    break;

#if !RETRO_USE_ORIGINAL_CODE
                case GLFW_KEY_F1:
                    if (engine.devMenu) {
                        sceneInfo.listPos--;
                        if (sceneInfo.listPos < sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart
                            || sceneInfo.listPos >= sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd) {
                            sceneInfo.activeCategory--;
                            if (sceneInfo.activeCategory >= sceneInfo.categoryCount) {
                                sceneInfo.activeCategory = sceneInfo.categoryCount - 1;
                            }
                            sceneInfo.listPos = sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd - 1;
                        }

#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5: LoadScene(); break;
                            case 4:
                            case 3: RSDK::Legacy::stageMode = RSDK::Legacy::STAGEMODE_LOAD; break;
                        }
#else
                        LoadScene();
#endif
                    }
                    break;

                case GLFW_KEY_F2:
                    if (engine.devMenu) {
                        sceneInfo.listPos++;
                        if (sceneInfo.listPos >= sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd || sceneInfo.listPos == 0) {
                            sceneInfo.activeCategory++;
                            if (sceneInfo.activeCategory >= sceneInfo.categoryCount) {
                                sceneInfo.activeCategory = 0;
                            }
                            sceneInfo.listPos = sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart;
                        }

#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5: LoadScene(); break;
                            case 4:
                            case 3: RSDK::Legacy::stageMode = RSDK::Legacy::STAGEMODE_LOAD; break;
                        }
#else
                        LoadScene();
#endif
                    }
                    break;
#endif

                case GLFW_KEY_F3:
                    if (userShaderCount)
                        videoSettings.shaderID = (videoSettings.shaderID + 1) % userShaderCount;
                    break;

#if !RETRO_USE_ORIGINAL_CODE
                case GLFW_KEY_F4:
                    if (engine.devMenu)
                        engine.showEntityInfo ^= 1;
                    break;

                case GLFW_KEY_F5:
                    if (engine.devMenu) {
                        // Quick-Reload
#if RETRO_USE_MOD_LOADER
                        if (mods & GLFW_MOD_CONTROL)
                            RefreshModFolders();
#endif

#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5: LoadScene(); break;
                            case 4:
                            case 3: RSDK::Legacy::stageMode = RSDK::Legacy::STAGEMODE_LOAD; break;
                        }
#else
                        LoadScene();
#endif
                    }
                    break;

                case GLFW_KEY_F6:
                    if (engine.devMenu && videoSettings.screenCount > 1)
                        videoSettings.screenCount--;
                    break;

                case GLFW_KEY_F7:
                    if (engine.devMenu && videoSettings.screenCount < SCREEN_COUNT)
                        videoSettings.screenCount++;
                    break;

                case GLFW_KEY_F8:
                    if (engine.devMenu)
                        engine.showUpdateRanges ^= 1;
                    break;

                case GLFW_KEY_F9:
                    if (engine.devMenu)
                        showHitboxes ^= 1;
                    break;

                case GLFW_KEY_F10:
                    if (engine.devMenu)
                        engine.showPaletteOverlay ^= 1;
                    break;
#endif
                case GLFW_KEY_BACKSPACE:
                    if (engine.devMenu)
                        engine.gameSpeed = engine.fastForwardSpeed;
                    break;

                case GLFW_KEY_F11:
                case GLFW_KEY_INSERT:
                    if (engine.devMenu)
                        engine.frameStep = true;
                    break;

                case GLFW_KEY_F12:
                case GLFW_KEY_PAUSE:
                    if (engine.devMenu) {
#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5:
                                if (sceneInfo.state != ENGINESTATE_NONE)
                                    sceneInfo.state ^= ENGINESTATE_STEPOVER;
                                break;
                            case 4:
                            case 3:
                                if (RSDK::Legacy::stageMode != ENGINESTATE_NONE)
                                    RSDK::Legacy::stageMode ^= RSDK::Legacy::STAGEMODE_STEPOVER;
                                break;
                        }
#else
                        if (sceneInfo.state != ENGINESTATE_NONE)
                            sceneInfo.state ^= ENGINESTATE_STEPOVER;
#endif
                    }
                    break;
            }
            break;
        }
        case GLFW_RELEASE: {
#if !RETRO_REV02
            --RSDK::SKU::buttonDownCount;
#endif
            switch (key) {
                default:
#if RETRO_INPUTDEVICE_KEYBOARD
                    SKU::ClearKeyState(key);
#endif
                    break;

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                case GLFW_KEY_ESCAPE:
                    RSDK::SKU::specialKeyStates[0] = false;
                    SKU::ClearKeyState(key);
                    break;

                case GLFW_KEY_ENTER:
                    RSDK::SKU::specialKeyStates[1] = false;
                    SKU::ClearKeyState(key);
                    break;
#endif
                case GLFW_KEY_BACKSPACE: engine.gameSpeed = 1; break;
            }
            break;
        }
    }
}
void RSDK::RenderDevice::ProcessFocusEvent(GLFWwindow *, int32 focused)
{
    if (!focused) {
#if RETRO_REV02
        SKU::userCore->focusState = 1;
#endif
    }
    else {
#if RETRO_REV02
        SKU::userCore->focusState = 0;
#endif
    }
}
void RSDK::RenderDevice::ProcessMouseEvent(GLFWwindow *, int32 button, int32 action, int32 mods)
{
    switch (action) {
        case GLFW_PRESS: {
            switch (button) {
                case GLFW_MOUSE_BUTTON_LEFT: touchInfo.down[0] = true; touchInfo.count = 1;
#if !RETRO_REV02
                    RSDK::SKU::buttonDownCount++;
#endif
                    break;

                case GLFW_MOUSE_BUTTON_RIGHT:
#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[3] = true;
                    RSDK::SKU::buttonDownCount++;
#endif
                    break;
            }
            break;
        }

        case GLFW_RELEASE: {
            switch (button) {
                case GLFW_MOUSE_BUTTON_LEFT: touchInfo.down[0] = false; touchInfo.count = 0;
#if !RETRO_REV02
                    RSDK::SKU::buttonDownCount--;
#endif
                    break;

                case GLFW_MOUSE_BUTTON_RIGHT:
#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[3] = false;
                    RSDK::SKU::buttonDownCount--;
#endif
                    break;
            }
            break;
        }
    }
}
void RSDK::RenderDevice::ProcessJoystickEvent(int32 ID, int32 event)
{
#if RETRO_INPUTDEVICE_GLFW
    if (!glfwJoystickIsGamepad(ID))
        return;
    uint32 hash;
    char idBuffer[0x20];
    sprintf_s(idBuffer, sizeof(idBuffer), "%s%d", "GLFWDevice", ID);
    GenerateHashCRC(&hash, idBuffer);

    if (event == GLFW_CONNECTED)
        SKU::InitGLFWInputDevice(hash, ID);
    else
        RemoveInputDevice(InputDeviceFromID(hash));
#endif
}
void RSDK::RenderDevice::ProcessMaximizeEvent(GLFWwindow *, int32 maximized)
{
    // i don't know why this is a thing
    if (maximized) {
        // set fullscreen idk about the specifics rn
    }
}

void RSDK::RenderDevice::SetLinear(bool32 linear)
{
    for (int32 i = 0; i < SCREEN_COUNT; ++i) {
        glBindTexture(GL_TEXTURE_2D, screenTextures[i]);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    }
}

#if EXTRA_HW_RENDER

#define VERTEX_LIMIT (0x4000) // absolute max i don't think i need more

GLuint hwVAO;
GLuint hwVBO;
uint32 vboOff;
GLuint hwIBO;
uint32 iboOff;

GLuint screenFB[SCREEN_COUNT];
GLuint fbVBO;

float *attributeBuf;
GLuint layerTextures[LAYER_COUNT];
GLuint attribTextures[LAYER_COUNT + 1]; // LAST IS USED FOR SPRITES AND OTHER THINGS THAT ONLY USE LINEBUF
GLuint paletteTex;

GLuint tFBT;
GLuint tFB;

uint16 quadIndices[VERTEX_LIMIT / 4 * 6];

const RenderVertex fullVerts[] = {
    { { -1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 0.0 } }, { { -1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 1.0 } },
    { { +1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 1.0 } }, { { -1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 0.0 } },
    { { +1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 0.0 } }, { { +1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 1.0 } },
};

RenderVertex tileVerts[] = {
    { { -1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 0.0 } }, { { -1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 1.0 } },
    { { +1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 1.0 } }, { { -1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 0.0 } },
    { { +1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 0.0 } }, { { +1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 1.0 } },
};

RenderVertex placeVerts[] = {
    { { -1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 0.0 } }, { { -1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 1.0 } },
    { { +1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 1.0 } }, { { -1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 0.0, 0.0 } },
    { { +1.0, -1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 0.0 } }, { { +1.0, +1.0, 1.0 }, 0xFFFFFFFF, { 1.0, 1.0 } },
};

void Shader::Use() { glUseProgram((GLuint)internal); };
void Shader::SetUniformI(const char *name, int32 value) { glUniform1i(glGetUniformLocation((GLuint)internal, name), value); };
void Shader::SetUniformI2(const char *name, int32 x, int32 y) { glUniform2i(glGetUniformLocation((GLuint)internal, name), x, y); };
void Shader::SetUniformF(const char *name, float value) { glUniform1f(glGetUniformLocation((GLuint)internal, name), value); };
void Shader::SetUniformF2(const char *name, float x, float y) { glUniform2f(glGetUniformLocation((GLuint)internal, name), x, y); };
void Shader::SetTexture(const char *name, void *tex) { SetUniformI(name, (GLuint)tex); }

Shader *RSDK::GetFBShader(int32 ink, float alpha)
{
    switch (ink) {
        case INK_NONE: return (Shader *)new FBNoneShader(fbNoneShader);
        case INK_BLEND: return (Shader *)new FBBlendShader(fbBlendShader);
        case INK_ALPHA: {
            auto shader   = new FBAlphaShader(fbAlphaShader);
            shader->alpha = alpha;
            return (Shader *)shader;
        }
        case INK_ADD: {
            auto shader       = new FBAddShader(fbAddShader);
            shader->intensity = alpha;
            return (Shader *)shader;
        }
        case INK_SUB: {
            auto shader       = new FBSubShader(fbSubShader);
            shader->intensity = alpha;
            return (Shader *)shader;
        }
        case INK_TINT: {
            auto shader         = new FBTintShader(fbTintShader);
            shader->lookupTable = tintLookupTable;
            return (Shader *)shader;
        }
        case INK_MASKED: {
            auto shader   = new FBMaskedShader(fbMaskedShader);
            shader->color = maskColor;
            return (Shader *)shader;
        }
        case INK_UNMASKED: {
            auto shader   = new FBUnmaskedShader(fbUnmaskedShader);
            shader->color = maskColor;
            return (Shader *)shader;
        }
    }
    return nullptr;
}

struct TileShader : public Shader {
    uint16 palette[PALETTE_BANK_COUNT][PALETTE_BANK_SIZE];
    uint8 layer;

    void SetArgs()
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gfxSurface[0].texture);
        SetTexture("tileset", (void *)0);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, paletteTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PALETTE_BANK_SIZE, PALETTE_BANK_COUNT, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, palette);
        SetTexture("palette", (void *)1);
        glActiveTexture(GL_TEXTURE20);
        glBindTexture(GL_TEXTURE_2D, attribTextures[layer]);
        SetTexture("attribs", (void *)20);
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, layerTextures[layer]);
        SetTexture("tileLayout", (void *)10);
        SetUniformF2("pixelSize", RSDK::RenderDevice::pixelSize.x, RSDK::RenderDevice::pixelSize.y);
        SetUniformF2("layerSize", tileLayers[layer].xsize, tileLayers[layer].ysize);
    }
} tileDShader, tileHShader, tileVShader;

struct FillShader : public Shader {
    float aR, aG, aB;

    void SetArgs() { glUniform3f(glGetUniformLocation((GLuint)internal, "alpha"), aR, aG, aB); }
} fillShader;

void RectShader::SetArgs() {}
void CircleShader::SetArgs() { SetUniformF("radius", innerRadius); }

void SpriteShader::SetArgs()
{
    SetUniformI("tex", 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, paletteTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PALETTE_BANK_SIZE, PALETTE_BANK_COUNT, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, palette);
    SetTexture("palette", (void *)1);

    uint8 *lineBuffer = &gfxLineBuffer[currentScreen->clipBound_Y1];
    float *attBuf     = attributeBuf - 1;

    for (int32 cy = 0; cy < videoSettings.pixHeight; ++cy) {
        ++attBuf;
        ++attBuf;
        *++attBuf = *lineBuffer;
        ++attBuf;

        if (cy >= currentScreen->clipBound_Y1 && cy < currentScreen->clipBound_Y2) {
            ++lineBuffer;
        }
    }

    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, attribTextures[LAYER_COUNT]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, videoSettings.pixHeight, GL_RGBA, GL_FLOAT, attributeBuf);
    SetTexture("attribs", (void *)2);

    SetUniformF2("viewSize", RSDK::RenderDevice::viewSize.x, RSDK::RenderDevice::viewSize.y);
}
void DevTextShader::SetArgs() { SetUniformI("tex", 0); }

void FBNoneShader::SetArgs()
{
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // since alpha's normally handled seperately, this lets stuff blend nicely including D layers
}
void FBBlendShader::SetArgs()
{
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SetUniformF("alpha", 0.5);
}
void FBAlphaShader::SetArgs()
{
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SetUniformF("alpha", alpha);
}
void FBAddShader::SetArgs()
{
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    SetUniformF("alpha", intensity);
}
void FBSubShader::SetArgs()
{
    glBlendEquation(GL_FUNC_SUBTRACT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    SetUniformF("alpha", intensity);
}
void FBTintShader::SetArgs() {}
void FBMaskedShader::SetArgs() {}
void FBUnmaskedShader::SetArgs() {}

RectShader RSDK::rectShader;
CircleShader RSDK::circleShader;

DevTextShader RSDK::devTextShader;
SpriteShader RSDK::spriteShader;

FBNoneShader RSDK::fbNoneShader;
FBBlendShader RSDK::fbBlendShader;
FBAlphaShader RSDK::fbAlphaShader;
FBAddShader RSDK::fbAddShader;
FBSubShader RSDK::fbSubShader;
FBTintShader RSDK::fbTintShader;
FBMaskedShader RSDK::fbMaskedShader;
FBUnmaskedShader RSDK::fbUnmaskedShader;

RenderVertex *vertMap;
uint16 *indexMap;

RenderVertex *RSDK::AllocateVertexBuffer(uint32 count)
{
    assert((count <= VERTEX_LIMIT));
    if ((vboOff + count) > (VERTEX_LIMIT + 6))
        RSDK::RenderDevice::CopyFrameBuffer();
    if (GLEW_ARB_buffer_storage) {
        return vertMap + vboOff;
    }
    else {
        RenderVertex *r = (RenderVertex *)glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)(vboOff * sizeof(RenderVertex)),
                                                           (GLsizeiptr)(count * sizeof(RenderVertex)), GL_MAP_WRITE_BIT);
        GLenum err      = glGetError();
        return r;
    }
}
uint16 *RSDK::AllocateIndexBuffer(uint32 count)
{
    assert((count <= VERTEX_LIMIT));
    if ((iboOff + count) > VERTEX_LIMIT)
        RSDK::RenderDevice::CopyFrameBuffer();
    if (GLEW_ARB_buffer_storage) {
        return indexMap + iboOff;
    }
    else {
        uint16 *r  = (uint16 *)glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)(iboOff * sizeof(uint16)), (GLsizeiptr)(count * sizeof(uint16)),
                                               GL_MAP_WRITE_BIT);
        GLenum err = glGetError();
        return r;
    }
}

void RSDK::AddQuadsToBuffer(uint16 *indexBuffer, uint32 count) { memcpy(indexBuffer, quadIndices, count * 6 * sizeof(uint16)); }

void RSDK::SetupGFXSurface(GFXSurface *surface)
{
    glGenTextures(1, &surface->texture);
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1 << surface->lineSize, surface->height, 0, GL_RED, GL_UNSIGNED_BYTE, surface->pixels);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);

    if (GLEW_KHR_debug) {
        uint32 gfx  = surface - gfxSurface;
        char name[] = "gfxTex00";

        name[sizeof(name) - 3] = '0' + (gfx / 10);
        name[sizeof(name) - 2] = '0' + (gfx % 10);

        glObjectLabel(GL_TEXTURE, surface->texture, sizeof(name), name);
    }
}

void RSDK::RemoveGFXSurface(GFXSurface *surface) { glDeleteTextures(1, &surface->texture); }

void RSDK::PopulateTilesTexture()
{
    GFXSurface *surface = &gfxSurface[0];

    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, TILE_SIZE * FLIP_NONE, 0, TILE_SIZE, TILE_SIZE * TILE_COUNT, GL_RED, GL_UNSIGNED_BYTE,
                    &tilesetPixels[TILESET_SIZE * FLIP_NONE]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, TILE_SIZE * FLIP_X, 0, TILE_SIZE, TILE_SIZE * TILE_COUNT, GL_RED, GL_UNSIGNED_BYTE,
                    &tilesetPixels[TILESET_SIZE * FLIP_X]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, TILE_SIZE * FLIP_Y, 0, TILE_SIZE, TILE_SIZE * TILE_COUNT, GL_RED, GL_UNSIGNED_BYTE,
                    &tilesetPixels[TILESET_SIZE * FLIP_Y]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, TILE_SIZE * FLIP_XY, 0, TILE_SIZE, TILE_SIZE * TILE_COUNT, GL_RED, GL_UNSIGNED_BYTE,
                    &tilesetPixels[TILESET_SIZE * FLIP_XY]);
}

void RebindVAP()
{
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), 0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex), (void *)offsetof(RenderVertex, color));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), (void *)offsetof(RenderVertex, tex));
}

bool SetupHWRendering()
{
    GFXSurface *surface = &gfxSurface[0];

    glGenTextures(1, &surface->texture);
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TILE_SIZE * 4, TILE_SIZE * TILE_COUNT, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);

    if (GLEW_KHR_debug)
        glObjectLabel(GL_TEXTURE, surface->texture, -1, "tileTex");
    surface = &gfxSurface[1];

    glGenTextures(1, &surface->texture);
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 1 << surface->lineSize, surface->height, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, surface->pixels);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);

    if (GLEW_KHR_debug)
        glObjectLabel(GL_TEXTURE, surface->texture, -1, "devText");

    int vID = 0;
    for (int i = 0; i < (VERTEX_LIMIT / 4); i++) {
        quadIndices[vID++] = (i << 2) + 0;
        quadIndices[vID++] = (i << 2) + 1;
        quadIndices[vID++] = (i << 2) + 2;
        quadIndices[vID++] = (i << 2) + 1;
        quadIndices[vID++] = (i << 2) + 3;
        quadIndices[vID++] = (i << 2) + 2;
    }

    /*
    for (int i = 0; i < TILE_COUNT; ++i) {
        int o              = i * 4;
        tileUVArray[o + 0] = 0.f;
        tileUVArray[o + 1] = i / (float)TILE_COUNT;
        tileUVArray[o + 2] = 1.f;
        tileUVArray[o + 3] = (i + 1) / (float)TILE_COUNT;

        // FLIP X
        o += TILE_COUNT * 4;
        tileUVArray[o + 0] = tileUVArray[i * 4 + 2];
        tileUVArray[o + 1] = tileUVArray[i * 4 + 1];
        tileUVArray[o + 2] = tileUVArray[i * 4 + 0];
        tileUVArray[o + 3] = tileUVArray[i * 4 + 3];

        // FLIP Y
        o += TILE_COUNT * 4;
        tileUVArray[o + 0] = tileUVArray[i * 4 + 0];
        tileUVArray[o + 1] = tileUVArray[i * 4 + 3];
        tileUVArray[o + 2] = tileUVArray[i * 4 + 2];
        tileUVArray[o + 3] = tileUVArray[i * 4 + 1];

        // FLIP XY
        o += TILE_COUNT * 4;
        tileUVArray[o + 0] = tileUVArray[i * 4 + 2];
        tileUVArray[o + 1] = tileUVArray[i * 4 + 3];
        tileUVArray[o + 2] = tileUVArray[i * 4 + 0];
        tileUVArray[o + 3] = tileUVArray[i * 4 + 1];
    } //*/
    glGenVertexArrays(1, &hwVAO);
    glBindVertexArray(hwVAO);
    if (GLEW_KHR_debug)
        glObjectLabel(GL_VERTEX_ARRAY, hwVAO, -1, "hwVAO");

    RenderVertex vboBase[VERTEX_LIMIT + 6];

    memcpy(vboBase, fullVerts, sizeof(fullVerts));

    glGenBuffers(1, &hwVBO);
    glBindBuffer(GL_ARRAY_BUFFER, hwVBO);
    glGenBuffers(1, &hwIBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, hwIBO);
    if (GLEW_KHR_debug)
        glObjectLabel(GL_BUFFER, hwVBO, -1, "hwVBO");
    if (GLEW_KHR_debug)
        glObjectLabel(GL_BUFFER, hwIBO, -1, "hwIBO");

    if (GLEW_ARB_buffer_storage) {
        const int32 flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glBufferStorage(GL_ARRAY_BUFFER, sizeof(RenderVertex) * (VERTEX_LIMIT + 6), vboBase, flags);
        glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint16) * VERTEX_LIMIT, NULL, flags);

        vertMap  = (RenderVertex *)glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(RenderVertex) * (VERTEX_LIMIT + 6), flags);
        indexMap = (uint16 *)glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(uint16) * VERTEX_LIMIT, flags);
    }
    else {
        glBufferData(GL_ARRAY_BUFFER, sizeof(RenderVertex) * (VERTEX_LIMIT + 6), vboBase, GL_DYNAMIC_DRAW);
        uint16 iboBase[VERTEX_LIMIT];
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint16) * VERTEX_LIMIT, iboBase, GL_DYNAMIC_DRAW);
    }

    vboOff = 6;

    RebindVAP();
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    attributeBuf = (float *)malloc(videoSettings.pixWidth * videoSettings.pixHeight * sizeof(float) * 4);

    glGenTextures(LAYER_COUNT + 1, attribTextures);
    glGenTextures(LAYER_COUNT, layerTextures);

    if (GLEW_KHR_debug) {
        char lname[] = "layerTex0";
        char aname[] = "attrTex0";
        for (int l = 0; l < LAYER_COUNT; ++l) {
            lname[sizeof(lname) - 2] = '0' + l;
            aname[sizeof(aname) - 2] = '0' + l;
            glObjectLabel(GL_TEXTURE, layerTextures[l], sizeof(lname), lname);
            glObjectLabel(GL_TEXTURE, attribTextures[l], sizeof(aname), aname);
        }

        glObjectLabel(GL_TEXTURE, attribTextures[LAYER_COUNT], -1, "gfxLineTex");
    }

    glGenTextures(1, &paletteTex);
    glBindTexture(GL_TEXTURE_2D, paletteTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, PALETTE_BANK_SIZE, PALETTE_BANK_COUNT, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); // this lets cool stuff happen at higher res
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (GLEW_KHR_debug) {
        glObjectLabel(GL_TEXTURE, paletteTex, sizeof("paletteTex"), "paletteTex");
    }

    int32 height = pow(2, ceil(log(videoSettings.pixHeight) / log(2)));
    glBindTexture(GL_TEXTURE_2D, attribTextures[LAYER_COUNT]);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, height, 0, GL_RGBA, GL_FLOAT, NULL);

    glGenFramebuffers(1, &tFB);
    glGenTextures(1, &tFBT);

    glBindFramebuffer(GL_FRAMEBUFFER, tFB);
    glBindTexture(GL_TEXTURE_2D, tFBT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, RSDK::RenderDevice::textureSize.x, RSDK::RenderDevice::textureSize.y, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tFBT, 0);

    if (GLEW_KHR_debug)
        glObjectLabel(GL_TEXTURE, tFBT, sizeof("tFBT"), "tFBT");

    glGenFramebuffers(SCREEN_COUNT, screenFB);
    for (int32 i = 0; i < SCREEN_COUNT; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, screenFB[i]);
        glBindTexture(GL_TEXTURE_2D, RSDK::RenderDevice::screenTextures[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, RSDK::RenderDevice::screenTextures[i], 0);

        if (GLEW_KHR_debug) {
            char name[]            = "screenTex0";
            name[sizeof(name) - 2] = '0' + i;
            glObjectLabel(GL_TEXTURE, RSDK::RenderDevice::screenTextures[i], -1, name);
        }
    }

#define _LOAD_SHADER(name, vert, frag)                                                                                                               \
    name.internal = (void *)GL_LoadShader(vert, frag);                                                                                               \
    if (GLEW_KHR_debug) {                                                                                                                            \
        glObjectLabel(GL_PROGRAM, (GLuint)name.internal, -1, #name);                                                                                 \
    }

    _LOAD_SHADER(tileDShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/Tiles/TilesDeform.fs");
    _LOAD_SHADER(tileHShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/Tiles/TilesH.fs");
    _LOAD_SHADER(tileVShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/Tiles/TilesV.fs");

    _LOAD_SHADER(rectShader, "Data/Shaders/OGL/HW/Screensize.vs", "Data/Shaders/OGL/HW/Place/Color.fs");
    _LOAD_SHADER(circleShader, "Data/Shaders/OGL/HW/Screensize.vs", "Data/Shaders/OGL/HW/Place/Circle.fs");

    _LOAD_SHADER(devTextShader, "Data/Shaders/OGL/HW/Screensize.vs", "Data/Shaders/OGL/HW/Place/DevText.fs");
    _LOAD_SHADER(spriteShader, "Data/Shaders/OGL/HW/Screensize.vs", "Data/Shaders/OGL/HW/Place/Sprite.fs");

    _LOAD_SHADER(fbNoneShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/None.fs");
    _LOAD_SHADER(fbBlendShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Alpha.fs");
    _LOAD_SHADER(fbAlphaShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Alpha.fs");
    _LOAD_SHADER(fbAddShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Alpha.fs");
    _LOAD_SHADER(fbSubShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Alpha.fs");
    _LOAD_SHADER(fbTintShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Tint.fs");
    _LOAD_SHADER(fbMaskedShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Masked.fs");
    _LOAD_SHADER(fbUnmaskedShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Unmasked.fs");

    _LOAD_SHADER(fillShader, "Data/Shaders/OGL/HW/Passthrough.vs", "Data/Shaders/OGL/HW/FB/Fill.fs");
#undef _LOAD_SHADER

    float x = 0.5 / (float)RSDK::RenderDevice::viewSize.x;
    float y = 0.5 / (float)RSDK::RenderDevice::viewSize.y;

    // ignore the last 6 verts, they're scaled to the 1024x512 textures already!
    int32 vertCount = sizeof(placeVerts);
    for (int32 v = 0; v < vertCount; ++v) {
        RenderVertex *vertex = &placeVerts[v];
        vertex->pos.x        = vertex->pos.x + x;
        vertex->pos.y        = vertex->pos.y - y;

        if (vertex->tex.x)
            vertex->tex.x = screens[0].size.x * (1.0 / RSDK::RenderDevice::textureSize.x);

        if (vertex->tex.y)
            vertex->tex.y = screens[0].size.y * (1.0 / RSDK::RenderDevice::textureSize.y);
    }

    for (int32 v = 0; v < vertCount; ++v) {
        RenderVertex *vertex = &tileVerts[v];

        // vertex->tex.x /= (RSDK::RenderDevice::textureSize.x / scaling.x) / screens[0].size.x;
        // vertex->tex.y /= (RSDK::RenderDevice::textureSize.y / scaling.y) / screens[0].size.y;
    }

    return true;
}

void PrepareHWPass() { glBindVertexArray(hwVAO); }

void RSDK::PrepareLayerTextures()
{
    for (int l = 0; l < LAYER_COUNT; ++l) {
        TileLayer *layer = &tileLayers[l];

        if (!layer->xsize || !layer->ysize)
            return;

        glBindTexture(GL_TEXTURE_2D, layerTextures[l]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 1 << layer->widthShift, 1 << layer->heightShift, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        int32 width  = pow(2, ceil(log(videoSettings.pixWidth) / log(2)));
        int32 height = pow(2, ceil(log(videoSettings.pixHeight) / log(2)));
        glBindTexture(GL_TEXTURE_2D, attribTextures[l]);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        switch (layer->type) {
            case LAYER_BASIC:
            case LAYER_HSCROLL: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, height, 0, GL_RGBA, GL_FLOAT, NULL); break;
            case LAYER_VSCROLL: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, 1, 0, GL_RGBA, GL_FLOAT, NULL); break;
            default:
            case LAYER_ROTOZOOM: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, NULL); break;
        }
    }
}

void RSDK::DrawLayerHScroll(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;

    ScanlineInfo *scanline = &scanlines[currentScreen->clipBound_Y1];
    float *attBuf          = attributeBuf - 1;
    uint8 *lineBuffer      = &gfxLineBuffer[currentScreen->clipBound_Y1];

    for (int32 cy = 0; cy < videoSettings.pixHeight; ++cy) {
        int32 posX = scanline->position.x;
        int32 posY = scanline->position.y;

        *++attBuf = posX / (float)(1UL << (20 + layer->widthShift));
        *++attBuf = posY / (float)(1UL << (20 + layer->heightShift));
        *++attBuf = *lineBuffer;
        ++attBuf;

        if (cy >= currentScreen->clipBound_Y1 && cy < currentScreen->clipBound_Y2) {
            ++lineBuffer;
            ++scanline;
        }
    }

    int32 s            = screens - currentScreen;
    RenderState *state = currentState + s;
    TileShader *shader = new TileShader(tileHShader);
    state->shader      = (Shader *)shader;
    state->fbShader    = (Shader *)new FBNoneShader(fbNoneShader);
    state->texture     = nullptr;

    uint32 l = layer - tileLayers;
    glBindTexture(GL_TEXTURE_2D, attribTextures[l]);
    int32 height = pow(2, ceil(log(videoSettings.pixHeight) / log(2)));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, videoSettings.pixHeight, GL_RGBA, GL_FLOAT, attributeBuf);

    glBindTexture(GL_TEXTURE_2D, layerTextures[l]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1 << layer->widthShift, 1 << layer->heightShift, GL_RED_INTEGER, GL_UNSIGNED_SHORT, layer->layout);

    memcpy(shader->palette, fullPalette, sizeof(fullPalette));
    shader->layer = l;

    state->vertexBuffer = AllocateVertexBuffer(4);
    state->indexBuffer  = AllocateIndexBuffer(6);
    state->vertexCount += 4;
    state->indexCount += 6;
    AddQuadsToBuffer(state->indexBuffer, 1);

    state->vertexBuffer[0] = { { -1.0, -1.0, 1.0 }, 0xFFFFFF, { 0.0, 0.0 } };
    state->vertexBuffer[1] = { { +1.0, -1.0, 1.0 }, 0xFFFFFF, { 1.0, 0.0 } };
    state->vertexBuffer[2] = { { -1.0, +1.0, 1.0 }, 0xFFFFFF, { 0.0, 1.0 } };
    state->vertexBuffer[3] = { { +1.0, +1.0, 1.0 }, 0xFFFFFF, { 1.0, 1.0 } };

    PushCurrentState(s);
}

void RSDK::DrawLayerVScroll(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;

    ScanlineInfo *scanline = &scanlines[currentScreen->clipBound_X1];
    float *attBuf          = attributeBuf - 1;
    uint8 *lineBuffer      = &gfxLineBuffer[0];

    for (int32 cx = 0; cx < videoSettings.pixWidth; ++cx) {
        int32 posX = scanline->position.x;
        int32 posY = scanline->position.y;

        *++attBuf = posX / (float)(1UL << (20 + layer->widthShift));
        *++attBuf = posY / (float)(1UL << (20 + layer->heightShift));
        *++attBuf = *lineBuffer;
        ++attBuf;

        if (cx >= currentScreen->clipBound_X1 && cx < currentScreen->clipBound_X2)
            ++scanline;
        if (cx < currentScreen->size.y)
            ++lineBuffer;
    }

    int32 s            = screens - currentScreen;
    RenderState *state = currentState + s;
    TileShader *shader = new TileShader(tileVShader);
    state->shader      = (Shader *)shader;
    state->fbShader    = (Shader *)new FBNoneShader(fbNoneShader);
    state->texture     = nullptr;

    uint32 l = layer - tileLayers;
    glBindTexture(GL_TEXTURE_2D, attribTextures[l]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, videoSettings.pixWidth, 1, GL_RGBA, GL_FLOAT, attributeBuf);

    glBindTexture(GL_TEXTURE_2D, layerTextures[l]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1 << layer->widthShift, 1 << layer->heightShift, GL_RED_INTEGER, GL_UNSIGNED_SHORT, layer->layout);

    memcpy(shader->palette, fullPalette, sizeof(fullPalette));
    shader->layer = l;

    state->vertexBuffer = AllocateVertexBuffer(4);
    state->indexBuffer  = AllocateIndexBuffer(6);
    state->vertexCount += 4;
    state->indexCount += 6;
    AddQuadsToBuffer(state->indexBuffer, 1);

    state->vertexBuffer[0] = { { -1.0, -1.0, 1.0 }, 0xFFFFFF, { 0.0, 0.0 } };
    state->vertexBuffer[1] = { { +1.0, -1.0, 1.0 }, 0xFFFFFF, { 1.0, 0.0 } };
    state->vertexBuffer[2] = { { -1.0, +1.0, 1.0 }, 0xFFFFFF, { 0.0, 1.0 } };
    state->vertexBuffer[3] = { { +1.0, +1.0, 1.0 }, 0xFFFFFF, { 1.0, 1.0 } };

    PushCurrentState(s);
    // hehe
    renderStates[s].last().clip_Y1 = 0;
    renderStates[s].last().clip_Y2 = currentScreen->size.y;
}

void RSDK::DrawLayerRotozoom(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;

    ScanlineInfo *scanline = &scanlines[currentScreen->clipBound_Y1];
    float *attBuf          = attributeBuf - 1;
    uint8 *lineBuffer      = &gfxLineBuffer[currentScreen->clipBound_Y1];

    for (int32 cy = 0; cy < videoSettings.pixHeight; ++cy) {
        int32 posX = scanline->position.x;
        int32 posY = scanline->position.y;

        for (int32 cx = 0; cx < videoSettings.pixWidth; ++cx) {
            *++attBuf = posX / (float)(1UL << (20 + layer->widthShift));
            *++attBuf = posY / (float)(1UL << (20 + layer->heightShift));
            *++attBuf = *lineBuffer;
            ++attBuf;

            if (cx >= currentScreen->clipBound_X1 && cx < currentScreen->clipBound_X2) {
                posX += scanline->deform.x;
                posY += scanline->deform.y;
            }
        }

        if (cy >= currentScreen->clipBound_Y1 && cy < currentScreen->clipBound_Y2) {
            ++lineBuffer;
            ++scanline;
        }
    }

    int32 s            = screens - currentScreen;
    RenderState *state = currentState + s;
    TileShader *shader = new TileShader(tileDShader);
    state->shader      = (Shader *)shader;
    state->fbShader    = (Shader *)new FBNoneShader(fbNoneShader);
    state->texture     = nullptr;

    uint32 l = layer - tileLayers;
    glBindTexture(GL_TEXTURE_2D, attribTextures[l]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, videoSettings.pixWidth, videoSettings.pixHeight, GL_RGBA, GL_FLOAT, attributeBuf);

    glBindTexture(GL_TEXTURE_2D, layerTextures[l]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1 << layer->widthShift, 1 << layer->heightShift, GL_RED_INTEGER, GL_UNSIGNED_SHORT, layer->layout);

    memcpy(shader->palette, fullPalette, sizeof(fullPalette));
    shader->layer = l;

    state->vertexBuffer = AllocateVertexBuffer(4);
    state->indexBuffer  = AllocateIndexBuffer(6);
    state->vertexCount += 4;
    state->indexCount += 6;
    AddQuadsToBuffer(state->indexBuffer, 1);

    state->vertexBuffer[0] = { { -1.0, -1.0, 1.0 }, 0xFFFFFF, { 0.0, 0.0 } };
    state->vertexBuffer[1] = { { +1.0, -1.0, 1.0 }, 0xFFFFFF, { 1.0, 0.0 } };
    state->vertexBuffer[2] = { { -1.0, +1.0, 1.0 }, 0xFFFFFF, { 0.0, 1.0 } };
    state->vertexBuffer[3] = { { +1.0, +1.0, 1.0 }, 0xFFFFFF, { 1.0, 1.0 } };

    PushCurrentState(s);
}

void RSDK::DrawLayerBasic(TileLayer *layer) { DrawLayerHScroll(layer); }

void RSDK::FillScreen(uint32 color, int32 alphaR, int32 alphaG, int32 alphaB)
{
    int32 s            = screens - currentScreen;
    RenderState *state = currentState + s;
    state->shader      = (Shader *)new RectShader(rectShader);
    auto fbShader      = new FillShader(fillShader);
    fbShader->aR       = CLAMP(alphaR, 0, 0xFF) / 255.f;
    fbShader->aG       = CLAMP(alphaG, 0, 0xFF) / 255.f;
    fbShader->aB       = CLAMP(alphaB, 0, 0xFF) / 255.f;
    state->fbShader    = (Shader *)fbShader;
    state->texture     = nullptr;

    state->indexBuffer  = AllocateIndexBuffer(6);
    state->vertexBuffer = AllocateVertexBuffer(4);

    state->vertexBuffer[0] = { { 0, 0, 1.0 }, color, { 0.0, 0.0 } };
    state->vertexBuffer[1] = { { (float)currentScreen->pitch, 0, 1.0 }, color, { 1.0, 0.0 } };
    state->vertexBuffer[2] = { { 0, (float)currentScreen->size.y, 1.0 }, color, { 0.0, 1.0 } };
    state->vertexBuffer[3] = { { (float)currentScreen->pitch, (float)currentScreen->size.y, 1.0 }, color, { 1.0, 1.0 } };

    state->vertexCount += 4;
    state->indexCount += 6;
    AddQuadsToBuffer(state->indexBuffer, 1);
    PushCurrentState(s);
}

void RSDK::Draw3DScene(uint16 sceneID)
{
    if (sceneID >= SCENE3D_COUNT || true)
        return;

    RSDK::RenderDevice::CopyFrameBuffer();

    Entity *entity = sceneInfo.entity;
    Scene3D *scn   = &scene3DList[sceneID];

    // Setup face buffer.
    // Each face's depth is an average of the depth of its vertices.
    Scene3DVertex *vertices = scn->vertices;
    Scene3DFace *faceBuffer = scn->faceBuffer;
    uint8 *faceVertCounts   = scn->faceVertCounts;

    int32 vertIndex  = 0;
    int32 indexCount = 0;
    for (int32 i = 0; i < scn->faceCount; ++i) {
        switch (*faceVertCounts) {
            default:
            case 1:
                faceBuffer->depth = vertices[0].z;
                vertices += *faceVertCounts;
                break;

            case 2:
                faceBuffer->depth = vertices[0].z >> 1;
                faceBuffer->depth += vertices[1].z >> 1;
                vertices += 2;
                break;

            case 3:
                faceBuffer->depth = vertices[0].z >> 1;
                faceBuffer->depth = (faceBuffer->depth + (vertices[1].z >> 1)) >> 1;
                faceBuffer->depth += vertices[2].z >> 1;
                vertices += 3;
                break;

            case 4:
                faceBuffer->depth = vertices[0].z >> 2;
                faceBuffer->depth += vertices[1].z >> 2;
                faceBuffer->depth += vertices[2].z >> 2;
                faceBuffer->depth += vertices[3].z >> 2;
                vertices += 4;
                break;
        }

        faceBuffer->index = vertIndex;
        vertIndex += *faceVertCounts;
        indexCount += *faceVertCounts * 3 - 6;

        ++faceBuffer;
        ++faceVertCounts;
    }

    int32 s            = screens - currentScreen;
    RenderState *state = currentState + s;
    state->shader      = (Shader *)new RectShader(rectShader);
    state->fbShader    = (Shader *)new FBNoneShader(fbNoneShader);
    state->texture     = nullptr;

    state->vertexBuffer = AllocateVertexBuffer(scn->vertexCount);
    state->indexBuffer  = AllocateIndexBuffer(indexCount);

    state->vertexCount = scn->vertexCount;
    state->indexCount  = indexCount;

    for (int32 v = 0; v < scn->vertexCount; ++v) {
        RenderVertex *buf   = state->vertexBuffer + v;
        Scene3DVertex *vert = scn->vertices + v;
        // buf->pos.x          = (vert->x / 2.f) - (currentScreen->position.x);
        // buf->pos.y          = (vert->y / 2.f) - (currentScreen->position.y);

        buf->pos.x = FROM_FIXED_F((vert->x << 8) - (currentScreen->position.x << 16));
        buf->pos.y = FROM_FIXED_F((vert->y << 8) - (currentScreen->position.y << 16));
        buf->pos.z = 1.0;
        buf->color = vert->color;
    }

    // Sort the face buffer. This is needed so that the faces don't overlap each other incorrectly when they're rendered.
    // This is an insertion sort, taken from here:
    // https://web.archive.org/web/20110108233032/http://rosettacode.org/wiki/Sorting_algorithms/Insertion_sort#C

    Scene3DFace *a = scn->faceBuffer;

    int i, j;
    Scene3DFace temp;

    for (i = 1; i < scn->faceCount; i++) {
        temp = a[i];
        j    = i - 1;
        while (j >= 0 && a[j].depth < temp.depth) {
            a[j + 1] = a[j];
            j -= 1;
        }
        a[j + 1] = temp;
    }

    // Finally, display the faces.

    uint8 *vertCnt = scn->faceVertCounts;
    uint32 index   = 0;

    switch (scn->drawMode) {
        default: break;

        case S3D_SOLIDCOLOR:
        case S3D_SOLIDCOLOR_SHADED:
        case S3D_SOLIDCOLOR_SHADED_BLENDED:
            for (int32 f = 0; f < scn->faceCount; ++f) {
                int32 faceIndex = scn->faceBuffer[f].index;
                int32 start     = index;
                for (int32 v = 0; v < *vertCnt; ++v) {
                    if (v < *vertCnt - 2) {
                        state->indexBuffer[index++] = v - 1 + faceIndex;
                        state->indexBuffer[index++] = v + 1 + faceIndex;
                        state->indexBuffer[index++] = v + 2 + faceIndex;
                    }
                }
                state->indexBuffer[start] = faceIndex;
                vertCnt++;
            }
            break; //*/
    }

    PushCurrentState(s);
}

void RSDK::PushCurrentState(int32 screen)
{
    // do optimizing later
    RenderState *state = currentState + screen;

    memcpy(&state->clip_X1, &screens[screen].clipBound_X1, sizeof(int32) * 4);
    state->clip_X2 -= state->clip_X1;
    state->clip_Y2 -= state->clip_Y1; // just make it easier on myself

    renderStates[screen].push(*state);

    vboOff += state->vertexCount;
    iboOff += state->indexCount;
    state->indexCount  = 0;
    state->vertexCount = 0;

    if (!GLEW_ARB_buffer_storage) {
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    }

    // SPECIAL CASE!!
    if (state->fbShader == &fbTintShader) {
        RSDK::RenderDevice::CopyFrameBuffer(); // do it instantly
    }
}

void RSDK::RenderDevice::CopyFrameBuffer()
{
    // this should ALWAYS already be bound
    // glBindVertexArray(hwVAO);
    glBindFramebuffer(GL_FRAMEBUFFER, tFB);
    glViewport(0, 0, videoSettings.pixWidth * scaling.x, videoSettings.pixHeight * scaling.y);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    uint32 vOff = 6, iOff = 0;
    for (int32 s = 0; s < videoSettings.screenCount; ++s) {
        RenderState first{};
        RenderState &last = first, current = renderStates[s].front();
        bool firstState = true;

        while (!renderStates[s].empty()) {
            if (firstState)
                firstState = false;
            else
                last = current;
            current = renderStates[s].pop();

            current.shader->Use();
            current.shader->SetArgs();
            current.shader->SetUniformF2("pixelSize", RSDK::RenderDevice::pixelSize.x, RSDK::RenderDevice::pixelSize.y);

            if (current.texture) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, current.texture->texture);
            }
            glDisable(GL_BLEND);
            glScissor(current.clip_X1 * scaling.x, current.clip_Y1 * scaling.y, current.clip_X2 * scaling.x, current.clip_Y2 * scaling.y);
            glDrawElementsBaseVertex(GL_TRIANGLES, current.indexCount, GL_UNSIGNED_SHORT, (void *)(iOff * sizeof(uint16)), vOff);
            vOff += current.vertexCount;
            iOff += current.indexCount;

            if (current.fbShader != fbNoneShader.internal || renderStates[s].empty()
                || (renderStates[s].front().fbShader && renderStates[s].front().fbShader->internal != fbNoneShader.internal)) {
                glBindFramebuffer(GL_FRAMEBUFFER, screenFB[s]);
                glViewport(0, 0, RSDK::RenderDevice::textureSize.x, RSDK::RenderDevice::textureSize.y);
                glEnable(GL_BLEND);
                glDisable(GL_SCISSOR_TEST);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, screenTextures[s]);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tFBT);
                current.fbShader->Use();
                current.fbShader->SetArgs();
                current.fbShader->SetUniformI("tex", 0);
                current.fbShader->SetUniformI("fb", 1);
                glDrawArrays(GL_TRIANGLES, 0, 6);

                delete current.shader;
                delete current.fbShader;

                glBindFramebuffer(GL_FRAMEBUFFER, tFB);
                glViewport(0, 0, videoSettings.pixWidth * scaling.x, videoSettings.pixHeight * scaling.y);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_SCISSOR_TEST);
            }
        }
        renderStates[s].finish();
    }
    vboOff = 6;
    iboOff = 0;
}
#endif