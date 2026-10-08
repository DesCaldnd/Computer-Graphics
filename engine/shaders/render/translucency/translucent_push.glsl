// Push constants of the translucent mesh passes (OX_RENDER_DRAW_PUSH: view, scene, drawIds + 32 bytes).
// flags: bit 0 = back-face depth valid.
OX_RENDER_DRAW_PUSH(uint refraction; uint refractionMips; uint sceneDepth; uint backDepth; uint fog; uint flags; float refractionStrength; float maxRefractionDistance;);
