#pragma once

#include <juce_graphics/juce_graphics.h>

/** Draws the app's logo (a rounded badge with an uneven response curve that ends flat)
    at any pixel size, so the tray icon stays sharp at every display scale.
    Matches Resources/src/tray.svg, which the .ico/.icns artwork is rendered from. */
namespace AppIcon
{
    enum class Style
    {
        colour,      // normal: warm orange-to-pink badge
        bypassed,    // grey badge, used while correction is bypassed
        monochrome   // black badge with the curve cut out (macOS menu-bar template)
    };

    /** The curve, in a 64 x 64 coordinate space. */
    inline juce::Path curve()
    {
        juce::Path p;
        p.startNewSubPath (10.0f, 33.0f);
        p.cubicTo (14.0f, 33.0f, 15.0f, 16.0f, 21.0f, 16.0f);
        p.cubicTo (27.0f, 16.0f, 27.0f, 48.0f, 33.0f, 48.0f);
        p.cubicTo (38.0f, 48.0f, 38.0f, 32.0f, 42.0f, 32.0f);
        p.lineTo (54.0f, 32.0f);
        return p;
    }

    inline juce::Image render (int size, Style style)
    {
        size = juce::jmax (8, size);
        const auto scale = (float) size / 64.0f;
        const juce::Rectangle<float> badge (1.0f, 1.0f, 62.0f, 62.0f);
        constexpr float corner = 14.0f;

        juce::Path stroke;
        juce::PathStrokeType (7.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
            .createStrokedPath (stroke, curve());

        juce::Image image (juce::Image::ARGB, size, size, true);

        {
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (scale));

            if (style == Style::monochrome)
            {
                g.setColour (juce::Colours::black);
            }
            else
            {
                const auto top    = style == Style::bypassed ? juce::Colour (0xff8a8f98) : juce::Colour (0xffff9a3c);
                const auto bottom = style == Style::bypassed ? juce::Colour (0xff5b6068) : juce::Colour (0xffe2336b);
                g.setGradientFill (juce::ColourGradient (top, badge.getTopLeft(), bottom, badge.getBottomRight(), false));
            }

            g.fillRoundedRectangle (badge, corner);

            if (style != Style::monochrome)
            {
                g.setColour (juce::Colours::white);
                g.fillPath (stroke);
            }
        }

        if (style == Style::monochrome)
        {
            // Knock the curve out of the badge so it reads as a template image.
            juce::Image mask (juce::Image::SingleChannel, size, size, true);

            {
                juce::Graphics g (mask);
                g.addTransform (juce::AffineTransform::scale (scale));
                g.setColour (juce::Colours::white);
                g.fillPath (stroke);
            }

            const juce::Image::BitmapData maskData (mask, juce::Image::BitmapData::readOnly);
            juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);

            for (int y = 0; y < size; ++y)
            {
                for (int x = 0; x < size; ++x)
                {
                    const auto cut  = maskData.getPixelColour (x, y).getFloatAlpha();
                    const auto base = data.getPixelColour (x, y).getFloatAlpha();
                    data.setPixelColour (x, y, juce::Colours::black.withAlpha (base * (1.0f - cut)));
                }
            }
        }

        return image;
    }
}
