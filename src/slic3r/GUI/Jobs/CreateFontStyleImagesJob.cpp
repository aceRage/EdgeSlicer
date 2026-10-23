#include "CreateFontStyleImagesJob.hpp"

// rasterization of ExPoly
#include "libslic3r/SLA/AGGRaster.hpp"
#include "slic3r/GUI/3DScene.hpp" // ::glsafe

// ability to request new frame after finish rendering
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include "FontStyleImageSize.hpp"

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Emboss;

// margin around the text of a style preview for the anti-aliasing, in pixels (OrcaSlicer #14612)
static constexpr int style_image_padding_px = 2;

CreateFontStyleImagesJob::CreateFontStyleImagesJob(StyleManager::StyleImagesData &&input)
    : m_input(std::move(input)), m_width(0), m_height(0)
{
    assert(m_input.result != nullptr);
    assert(!m_input.styles.empty());
    assert(!m_input.text.empty());
    assert(m_input.max_size.x() > 1);
    assert(m_input.max_size.y() > 1);
    assert(m_input.ppm > 1e-5);
}

void CreateFontStyleImagesJob::process(Ctl &ctl)
{    
    // create shapes and calc size (bounding boxes)
    std::vector<ExPolygons> name_shapes(m_input.styles.size());
    std::vector<double> scales(m_input.styles.size());
    m_images = std::vector<StyleManager::StyleImage>(m_input.styles.size());

    auto was_canceled = []() { return false; };
    for (auto &item : m_input.styles) {
        size_t index = &item - &m_input.styles.front();
        ExPolygons &shapes = name_shapes[index];
        shapes = text2shapes(item.font, m_input.text.c_str(), item.prop, was_canceled);

        // create image description
        StyleManager::StyleImage &image = m_images[index];
        BoundingBox &bounding_box = image.bounding_box;
        for (const ExPolygon &shape : shapes)
            bounding_box.merge(BoundingBox(shape.contour.points));

        // OrcaSlicer #14612: fit the text to the row height (not the style's font size and the screen DPI,
        // which made small styles unreadable and cropped large ones), then crop the width. See FontStyleImageSize.hpp.
        const Vec2d bb_size = bounding_box.defined ? bounding_box.size().cast<double>() : Vec2d(0., 0.);
        const StyleImageSize size = style_image_size(bb_size.x(), bb_size.y(), m_input.max_size.x(), m_input.max_size.y(),
                                                     style_image_padding_px);
        scales[index] = size.scale;
        image.tex_size.x = float(size.width);
        image.tex_size.y = float(size.height);
        if (!size.valid) {
            shapes.clear(); // nothing to draw (no glyphs)
            continue;
        }
        // margin for the anti-aliasing on the left and the top as well
        const coord_t padding = coord_t(std::round(style_image_padding_px / size.scale));
        for (ExPolygon &shape : shapes) shape.translate(Point(padding, padding) - bounding_box.min);
    }

    // arrange bounding boxes
    int offset_y = 0;
    m_width      = 0;
    for (StyleManager::StyleImage &image : m_images) {
        image.offset.y() = offset_y;
        offset_y += image.tex_size.y+1;
        if (m_width < image.tex_size.x) 
            m_width = image.tex_size.x;
    }
    m_height = offset_y;
    if (m_width < 1)
        m_width = 1; // every style without glyphs: keep the texture and the uv valid
    for (StyleManager::StyleImage &image : m_images) {
        const Point &o = image.offset;
        const ImVec2 &s = image.tex_size;
        image.uv0 = ImVec2(o.x() / (double) m_width, 
                           o.y() / (double) m_height);
        image.uv1 = ImVec2((o.x() + s.x) / (double) m_width,
                           (o.y() + s.y) / (double) m_height);
    }

    // Set up result
    m_pixels = std::vector<unsigned char>(4 * m_width * m_height, {255});

    // upload sub textures
    for (StyleManager::StyleImage &image : m_images) {
        size_t index = &image - &m_images.front();
        if (name_shapes[index].empty() || image.tex_size.x < 1 || image.tex_size.y < 1)
            continue;
        sla::Resolution resolution(image.tex_size.x, image.tex_size.y);
        double pixel_dim = SCALING_FACTOR / scales[index];
        sla::PixelDim dim(pixel_dim, pixel_dim);
        double gamma = 1.;
        std::unique_ptr<sla::RasterBase> r =
            sla::create_raster_grayscale_aa(resolution, dim, gamma);
        for (const ExPolygon &shape : name_shapes[index]) r->draw(shape);
        
        // copy rastered data to pixels
        sla::RasterEncoder encoder = [&offset = image.offset, &pix = m_pixels, w=m_width,h=m_height]
        (const void *ptr, size_t width, size_t height, size_t num_components) {
            // bigger value create darker image
            unsigned char gray_level = 1;
            size_t size {static_cast<size_t>(w*h)};
            assert((offset.x() + width) <= (size_t)w);
            assert((offset.y() + height) <= (size_t)h);
            const unsigned char *ptr2 = (const unsigned char *) ptr;
            for (size_t x = 0; x < width; ++x)
                for (size_t y = 0; y < height; ++y) { 
                    size_t index = (offset.y() + y)*w + offset.x() + x;
                    assert(index < size);
                    if (index >= size) continue;
                    pix[4*index+3] = ptr2[y * width + x] / gray_level;
                }
            return sla::EncodedRaster();
        };
        r->encode(encoder);
    }
}

void CreateFontStyleImagesJob::finalize(bool canceled, std::exception_ptr &)
{
    if (canceled) return;
    // upload texture on GPU
    GLuint tex_id;
    GLenum target = GL_TEXTURE_2D, format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    GLint  level = 0, border = 0;
    glsafe(::glGenTextures(1, &tex_id));
    glsafe(::glBindTexture(target, tex_id));
    glsafe(::glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GLint w = m_width, h = m_height;
    glsafe(::glTexImage2D(target, level, GL_RGBA, w, h, border, format, type,
                          (const void *) m_pixels.data()));

    // set up texture id
    void *texture_id = (void *) (intptr_t) tex_id;        
    for (StyleManager::StyleImage &image : m_images)
        image.texture_id = texture_id;
        
    // move to result
    m_input.result->styles = std::move(m_input.styles);
    m_input.result->images = std::move(m_images);

    // bind default texture
    GLuint no_texture_id = 0;
    glsafe(::glBindTexture(target, no_texture_id));
        
    // show rendered texture
    wxGetApp().plater()->canvas3D()->schedule_extra_frame(0);
}