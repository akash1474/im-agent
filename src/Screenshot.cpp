#ifdef AGENT_BRIDGE

#include "AgentInternal.h"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "imstb_image_write.h"

namespace
{
    std::string Base64(const std::string& in)
    {
        static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3)
        {
            const uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
            out += kAlphabet[v >> 18];
            out += kAlphabet[(v >> 12) & 63];
            out += kAlphabet[(v >> 6) & 63];
            out += kAlphabet[v & 63];
        }
        if (i + 1 == in.size())
        {
            const uint32_t v = (uint8_t)in[i] << 16;
            out += kAlphabet[v >> 18];
            out += kAlphabet[(v >> 12) & 63];
            out += "==";
        }
        else if (i + 2 == in.size())
        {
            const uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
            out += kAlphabet[v >> 18];
            out += kAlphabet[(v >> 12) & 63];
            out += kAlphabet[(v >> 6) & 63];
            out += '=';
        }
        return out;
    }

    void AppendToString(void* context, void* data, int size) { static_cast<std::string*>(context)->append((const char*)data, (size_t)size); }
} // namespace

namespace Agent
{
    bool CaptureFramebuffer(Capture& out)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const int w = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x + 0.5f);
        const int h = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y + 0.5f);
        if (w <= 0 || h <= 0)
            return false;

        auto pixels = std::make_shared<std::vector<uint8_t>>((size_t)w * h * 4);
        GLint previousAlignment = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        while (glGetError() != GL_NO_ERROR) {} // Clear errors left by the app so only the readback's is seen.
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels->data());
        const bool readOk = glGetError() == GL_NO_ERROR;
        glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);
        if (!readOk)
            return false;

        out.rgba = std::move(pixels);
        out.width = w;
        out.height = h;
        out.pixelsPerPoint = io.DisplayFramebufferScale.x;
        out.displaySize = io.DisplaySize;
        return true;
    }

    Json EncodeScreenshot(const Capture& c, const ShotOptions& o)
    {
        const float ppp = c.pixelsPerPoint;
        const float scale = std::clamp(o.scale, 0.1f, ppp);

        ImRect region(0.0f, 0.0f, c.displaySize.x, c.displaySize.y);
        if (o.hasRegion)
        {
            region.Min = ImMax(region.Min, o.region.Min);
            region.Max = ImMin(region.Max, o.region.Max);
        }

        // Source rectangle in framebuffer pixels, y measured from the top. Clamped as floats first: user-supplied
        // regions can be huge and an out-of-range float-to-int conversion is undefined.
        auto toPixel = [](float v, int limit) { return (int)std::clamp(v, 0.0f, (float)limit); };
        const int sx0 = toPixel(std::floor(region.Min.x * ppp), c.width);
        const int sy0 = toPixel(std::floor(region.Min.y * ppp), c.height);
        const int sx1 = toPixel(std::ceil(region.Max.x * ppp), c.width);
        const int sy1 = toPixel(std::ceil(region.Max.y * ppp), c.height);
        if (sx1 <= sx0 || sy1 <= sy0)
            return Call::Result(Call::TextItem("Region is empty or outside the window"), true);

        const float factor = ppp / scale; // Source pixels per output pixel.
        const int ow = std::max(1, (int)std::lround((sx1 - sx0) / factor));
        const int oh = std::max(1, (int)std::lround((sy1 - sy0) / factor));

        std::vector<uint8_t> rgb((size_t)ow * oh * 3);
        const uint8_t* src = c.rgba->data();
        for (int oy = 0; oy < oh; oy++)
        {
            const int y0 = sy0 + (int)std::floor(oy * factor);
            const int y1 = std::min(sy1, std::max(y0 + 1, sy0 + (int)std::floor((oy + 1) * factor)));
            for (int ox = 0; ox < ow; ox++)
            {
                const int x0 = sx0 + (int)std::floor(ox * factor);
                const int x1 = std::min(sx1, std::max(x0 + 1, sx0 + (int)std::floor((ox + 1) * factor)));
                uint32_t sum[3] = {0, 0, 0};
                int count = 0;
                for (int y = y0; y < y1; y++)
                {
                    const uint8_t* row = src + ((size_t)(c.height - 1 - y) * c.width) * 4; // Buffer rows are bottom-up.
                    for (int x = x0; x < x1; x++, count++)
                    {
                        sum[0] += row[x * 4];
                        sum[1] += row[x * 4 + 1];
                        sum[2] += row[x * 4 + 2];
                    }
                }
                uint8_t* dst = &rgb[((size_t)oy * ow + ox) * 3];
                if (count == 0)
                    continue;
                dst[0] = (uint8_t)(sum[0] / count);
                dst[1] = (uint8_t)(sum[1] / count);
                dst[2] = (uint8_t)(sum[2] / count);
            }
        }

        std::string png;
        if (!stbi_write_png_to_func(AppendToString, &png, ow, oh, 3, rgb.data(), ow * 3))
            return Call::Result(Call::TextItem("PNG encoding failed"), true);

        Json image = Json::object();
        image["type"] = "image";
        image["data"] = Base64(png);
        image["mimeType"] = "image/png";

        Json meta = Json::object();
        meta["width"] = ow;
        meta["height"] = oh;
        meta["scale"] = scale;
        meta["origin"] = Json::array({sx0 / ppp, sy0 / ppp});
        meta["note"] = "Image pixel (px,py) = ImGui point (origin.x + px/scale, origin.y + py/scale).";

        Json result = Call::Result(std::move(image), false);
        result["content"].push_back(Call::TextItem(Dump(meta)));
        return result;
    }
} // namespace Agent

#endif // AGENT_BRIDGE
