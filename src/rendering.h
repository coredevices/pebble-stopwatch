#pragma once

#include <pebble.h>


//! renders the LECO font onto the drawing context at the specified size
//! @param ctx the DrawingContext to draw the text onto
//! @param text the actual text to draw
//! @param size the size of the text array in bytes
//! @param font_size the font size to draw it at
//! @param position the position to draw at
void rendering_draw_text(GContext *ctx, char *text, uint8_t size, uint16_t font_size,
                         GPoint position);


//! gets the width a font will be when rendered
//! @param the text under question
//! @param the font size to measure the rendered size of
//! @return the rendered width of the font
uint32_t rendering_get_size(char *buff, uint8_t font_size);