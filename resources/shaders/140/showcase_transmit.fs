#version 140

// Showcase realism 1d (tests/design_showcase_realism.md 2.4): the coloured shadows. Every shadow
// caster is drawn from the key light into an RGBA8 map cleared to white with multiplying blending:
// an opaque one multiplies by black, a translucent one by the light it lets through, in its colour
// (Showcase::shadow_transmittance). The floor lightens and tints its shadow by this map
// (showcase_floor.fs), so a translucent filament throws a lighter, coloured shadow. Uses
// showcase_material.vs with the light's view-projection as the projection.

uniform vec3 transmit;   // linear, 1 = lets all light through, 0 = opaque

in vec4 world_pos;
in vec3 world_normal;

out vec4 out_color;

void main()
{
    if (world_pos.z < -0.001)
        discard;
    out_color = vec4(clamp(transmit, 0.0, 1.0), 1.0);
}
