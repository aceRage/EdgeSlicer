#include "MinimumSpanningTree.hpp"

#include <iterator>
#include <algorithm>
#include "libslic3r.h"

namespace Slic3r
{

#define unscale_(val) ((val) * SCALING_FACTOR)

inline double dot_with_unscale(const Point a, const Point b)
{
    return unscale_(a(0)) * unscale_(b(0)) + unscale_(a(1)) * unscale_(b(1));
}

inline double vsize2_with_unscale(const Point pt)
{
    return dot_with_unscale(pt, pt);
}

MinimumSpanningTree::MinimumSpanningTree(std::vector<Point> vertices) : adjacency_graph(prim(vertices))
{
    //Just copy over the fields.
}

auto MinimumSpanningTree::prim(std::vector<Point> vertices) const -> AdjacencyGraph_t
{
    AdjacencyGraph_t result;
    if (vertices.empty())
    {
        return result; //No vertices, so we can't create edges either.
    }
    // If there's only one vertex, we can't go creating any edges so just add the point to the adjacency list with no
    // edges
    if (vertices.size() == 1)
    {
        // unordered_map::operator[]() will construct an empty vector in place for us when we try and access an element
        // that doesnt exist
        result[*vertices.begin()];
        return result;
    }
    result.reserve(vertices.size());
    const std::vector<Point> &vertices_list = vertices;

    // The candidate set used to be two std::unordered_maps keyed by `const Point*`, and the vertex
    // to add next was picked with std::min_element over one of them. An unordered_map keyed by a
    // pointer iterates in the hash order of heap addresses, so every tie in the distance - and
    // ties are the normal case for support points sitting on a grid - was broken by where the
    // vertices happened to be allocated. That is why classic tree support put its branches in
    // different places from one slice of a project to the next, on the same binary and the same
    // input. Keep the candidates in a vector instead and break a tie on the coordinates (see the
    // selection below), which is a property of the points alone.
    // OrcaSlicer #17251: the vertices still outside the tree are kept in an index list, so each step
    // visits only those instead of every vertex (the arrays above were already index-based here).
    const size_t          n = vertices_list.size();
    std::vector<coordf_t> smallest_distance(n, 0.);    //The shortest distance to the current tree.
    std::vector<size_t>   smallest_distance_to(n, 0);  //Which vertex the shortest distance goes towards.
    std::vector<size_t>   candidates;                  //The vertices still outside the tree.
    candidates.reserve(n - 1);
    for (size_t vertex_index = 1; vertex_index < n; vertex_index++)
    {
        smallest_distance[vertex_index] = vsize2_with_unscale(vertices_list[vertex_index] - vertices_list[0]);
        candidates.push_back(vertex_index);
    }

    while (! candidates.empty()) //All of the vertices need to be in the tree at the end.
    {
        //Choose the closest vertex to connect to that is not yet in the tree.
        //This search is O(V) right now, which can be made down to O(log(V)). This reduces the overall time complexity from O(V*V) to O(V*log(E)).
        //However that requires an implementation of a heap that supports the decreaseKey operation, which is not in the std library.
        // Break equal-distance ties on the coordinates (Orca #15565), not on the position in the input:
        // the tree is then a property of the set of points alone, whatever order they were given in,
        // and of the order of the candidate list too.
        size_t best = 0;
        for (size_t candidate = 1; candidate < candidates.size(); candidate++)
        {
            const size_t a = candidates[candidate];
            const size_t b = candidates[best];
            if (smallest_distance[a] != smallest_distance[b] ? smallest_distance[a] < smallest_distance[b] : vertices_list[a] < vertices_list[b])
                best = candidate;
        }
        const size_t closest_index = candidates[best];
        candidates[best] = candidates.back(); //Remove it so we don't check for this point again.
        candidates.pop_back();

        //Add this point to the graph.
        const Point &closest_point = vertices_list[closest_index];
        const Point &other_end     = vertices_list[smallest_distance_to[closest_index]];
        result[closest_point].push_back({closest_point, other_end});
        result[other_end].push_back({other_end, closest_point});

        //Update the distances of all points that are not in the graph.
        for (const size_t i : candidates)
        {
            const coordf_t new_distance = vsize2_with_unscale(closest_point - vertices_list[i]);
            if (new_distance < smallest_distance[i]) //New point is closer.
            {
                smallest_distance[i]    = new_distance;
                smallest_distance_to[i] = closest_index;
            }
        }
    }

    return result;
}

std::vector<Point> MinimumSpanningTree::adjacent_nodes(Point node) const
{
    std::vector<Point> result;
    AdjacencyGraph_t::const_iterator adjacency_entry = adjacency_graph.find(node);
    if (adjacency_entry != adjacency_graph.end())
    {
        const auto& edges = adjacency_entry->second;
        std::transform(edges.begin(), edges.end(), std::back_inserter(result),
                       [&node](const Edge& e) { return (e.start == node) ? e.end : e.start; });
    }
    return result;
}

std::vector<Point> MinimumSpanningTree::leaves() const
{
    std::vector<Point> result;
    for (std::pair<Point, std::vector<Edge>> node : adjacency_graph)
    {
        if (node.second.size() <= 1) //Leaves are nodes that have only one adjacent edge, or just the one node if the tree contains one node.
        {
            result.push_back(node.first);
        }
    }
    return result;
}

std::vector<Point> MinimumSpanningTree::vertices() const
{
    std::vector<Point> result;
    using MapValue = std::pair<Point, std::vector<Edge>>;
    std::transform(adjacency_graph.begin(), adjacency_graph.end(), std::back_inserter(result),
                   [](const MapValue& node) { return node.first; });
    return result;
}

}
