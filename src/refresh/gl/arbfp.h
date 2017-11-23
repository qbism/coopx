static const char gl_prog_warp[] =
    "!!ARBfp1.0\n"
    "OPTION ARB_precision_hint_fastest;\n"

    "TEMP ofs, coord, diffuse;\n"
    "PARAM amp = { 0.0625, 0.0625 };\n"
    "PARAM phase = { 4, 4 };\n"

    "MAD coord, phase, fragment.texcoord[0], program.local[0];\n"
    "SIN ofs.x, coord.y;\n"
    "SIN ofs.y, coord.x;\n"
    "MAD coord, ofs, amp, fragment.texcoord[0];\n"
    "TEX diffuse, coord, texture[0], 2D;\n"
    "MUL result.color, diffuse, fragment.color;\n"
    "END\n"
;

static const char gl_prog_lightmapped[] =
    "!!ARBfp1.0\n"
    "OPTION ARB_precision_hint_fastest;\n"

    "TEMP diffuse, lightmap;\n"

    "TEX diffuse, fragment.texcoord[0], texture[0], 2D;\n"
    "TEX lightmap, fragment.texcoord[1], texture[1], 2D;\n"

    "MUL lightmap, lightmap, program.local[0];\n"

    "MUL diffuse, lightmap, diffuse;\n"
    "MUL result.color, diffuse, fragment.color;\n"

    "END\n"
;
