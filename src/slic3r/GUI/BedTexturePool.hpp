#ifndef slic3r_GUI_BedTexturePool_hpp_
#define slic3r_GUI_BedTexturePool_hpp_

#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// Rasterised bed-type textures kept across a bed re-layout (PartPlateList::load_bedtype_textures).
//
// A printer switch that changes the bed size (or the plate shapes) has to re-place the bed-type
// art: its rectangles scale with the bed. The images themselves only depend on the SVG file and
// the raster size, yet every re-layout used to delete and re-rasterise all of them (12 - 14 SVGs
// at 2048 px with a mipmap chain). The pool takes the old textures out of the parts before the
// re-layout and hands each one back to a new part that shows the same file; whatever is left
// (art that is no longer used, e.g. single- vs dual-nozzle layouts) is deleted by the caller.
// Owning raw pointers, to match BedTextureInfo::TexturePart.
template<class Texture>
class BedTexturePool
{
public:
    // Keep `tex` for a later part showing `filename`. Several parts may show the same file
    // (btPC and btPCT share their bottom art): each keeps its own texture, as before.
    void put(const std::string& filename, Texture* tex)
    {
        if (tex != nullptr)
            m_pool.emplace(filename, tex);
    }
    // A kept texture of `filename`, or nullptr when there is none (the caller then loads it).
    Texture* take(const std::string& filename)
    {
        auto it = m_pool.find(filename);
        if (it == m_pool.end())
            return nullptr;
        Texture* tex = it->second;
        m_pool.erase(it);
        return tex;
    }
    // Everything not handed back; the caller deletes these.
    std::vector<Texture*> drain()
    {
        std::vector<Texture*> out;
        out.reserve(m_pool.size());
        for (auto& kv : m_pool)
            out.push_back(kv.second);
        m_pool.clear();
        return out;
    }
    bool   empty() const { return m_pool.empty(); }
    size_t size() const { return m_pool.size(); }

private:
    std::multimap<std::string, Texture*> m_pool;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_BedTexturePool_hpp_
