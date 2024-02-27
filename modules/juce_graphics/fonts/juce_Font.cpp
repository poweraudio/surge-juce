/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

class Font::Native
{
public:
    HbFont font{};
};

namespace FontValues
{
    static float limitFontHeight (const float height) noexcept
    {
        return jlimit (0.1f, 10000.0f, height);
    }

    const float defaultFontHeight = 14.0f;
    float minimumHorizontalScale = 0.7f;
}

class HbScale
{
    static constexpr float factor = 1 << 16;

public:
    HbScale() = delete;

    static constexpr hb_position_t juceToHb (float pos)
    {
        return (hb_position_t) (pos * factor);
    }

    static constexpr float hbToJuce (hb_position_t pos)
    {
        return (float) pos / (float) factor;
    }
};

using GetTypefaceForFont = Typeface::Ptr (*)(const Font&);
GetTypefaceForFont juce_getTypefaceForFont = nullptr;

float Font::getDefaultMinimumHorizontalScaleFactor() noexcept                { return FontValues::minimumHorizontalScale; }
void Font::setDefaultMinimumHorizontalScaleFactor (float newValue) noexcept  { FontValues::minimumHorizontalScale = newValue; }

//==============================================================================
#if JUCE_MAC || JUCE_IOS
template <CTFontOrientation orientation>
void getAdvancesForGlyphs (hb_font_t* hbFont, CTFontRef ctFont, Span<const CGGlyph> glyphs, Span<CGSize> advances)
{
    jassert (glyphs.size() == advances.size());

    int x, y;
    hb_font_get_scale (hbFont, &x, &y);
    const auto scaleAdjustment = HbScale::hbToJuce (orientation == kCTFontOrientationHorizontal ? x : y) / CTFontGetSize (ctFont);

    CTFontGetAdvancesForGlyphs (ctFont, orientation, std::data (glyphs), std::data (advances), (CFIndex) std::size (glyphs));

    for (auto& advance : advances)
        (orientation == kCTFontOrientationHorizontal ? advance.width : advance.height) *= scaleAdjustment;
}

template <CTFontOrientation orientation>
static auto getAdvanceFn()
{
    return [] (hb_font_t* f, void*, hb_codepoint_t glyph, void* voidFontRef) -> hb_position_t
    {
        auto* fontRef = static_cast<CTFontRef> (voidFontRef);

        const CGGlyph glyphs[] { (CGGlyph) glyph };
        CGSize advances[std::size (glyphs)]{};
        getAdvancesForGlyphs<orientation> (f, fontRef, glyphs, advances);

        return HbScale::juceToHb ((float) (orientation == kCTFontOrientationHorizontal ? advances->width : advances->height));
    };
}

template <CTFontOrientation orientation>
static auto getAdvancesFn()
{
    return [] (hb_font_t* f,
               void*,
               unsigned int count,
               const hb_codepoint_t* firstGlyph,
               unsigned int glyphStride,
               hb_position_t* firstAdvance,
               unsigned int advanceStride,
               void* voidFontRef)
    {
        auto* fontRef = static_cast<CTFontRef> (voidFontRef);

        std::vector<CGGlyph> glyphs (count);

        for (auto [index, glyph] : enumerate (glyphs))
            glyph = (CGGlyph) *addBytesToPointer (firstGlyph, glyphStride * index);

        std::vector<CGSize> advances (count);

        getAdvancesForGlyphs<orientation> (f, fontRef, glyphs, advances);

        for (auto [index, advance] : enumerate (advances))
            *addBytesToPointer (firstAdvance, advanceStride * index) = HbScale::juceToHb ((float) (orientation == kCTFontOrientationHorizontal ? advance.width : advance.height));
    };
}

/*  This function overrides the callbacks that fetch glyph advances for fonts on macOS.
    The built-in OpenType glyph metric callbacks that HarfBuzz uses by default for fonts such as
    "Apple Color Emoji" don't always return correct advances, resulting in emoji that may overlap
    with subsequent characters. This may be to do with ignoring the 'trak' table, but I'm not an
    expert, so I'm not sure!

    In any case, using CTFontGetAdvancesForGlyphs produces much nicer advances for emoji on Apple
    platforms, as long as the CTFont is set to the size that will eventually be rendered.

    This might need a bit of testing to make sure that it correctly handles advances for
    custom (non-Apple?) fonts.

    @param hb       a hb_font_t to update with Apple-specific advances
    @param fontRef  the CTFontRef (normally with a custom point size) that will be queried when computing advances
*/
static void overrideCTFontAdvances (hb_font_t* hb, CTFontRef fontRef)
{
    using HbFontFuncs = std::unique_ptr<hb_font_funcs_t, FunctionPointerDestructor<hb_font_funcs_destroy>>;
    const HbFontFuncs funcs { hb_font_funcs_create() };

    // We pass the CTFontRef as user data to each of these functions.
    // We don't pass a custom destructor for the user data, as that will be handled by the custom
    // destructor for the hb_font_funcs_t.
    hb_font_funcs_set_glyph_h_advance_func  (funcs.get(), getAdvanceFn <kCTFontOrientationHorizontal>(), (void*) fontRef, nullptr);
    hb_font_funcs_set_glyph_v_advance_func  (funcs.get(), getAdvanceFn <kCTFontOrientationVertical>(),   (void*) fontRef, nullptr);
    hb_font_funcs_set_glyph_h_advances_func (funcs.get(), getAdvancesFn<kCTFontOrientationHorizontal>(), (void*) fontRef, nullptr);
    hb_font_funcs_set_glyph_v_advances_func (funcs.get(), getAdvancesFn<kCTFontOrientationVertical>(),   (void*) fontRef, nullptr);

    // We want to keep a copy of the font around so that all of our custom callbacks can query it,
    // so retain it here and release it once the custom functions are no longer in use.
    jassert (fontRef != nullptr);
    CFRetain (fontRef);

    hb_font_set_funcs (hb, funcs.get(), (void*) fontRef, [] (void* ptr)
    {
        CFRelease ((CTFontRef) ptr);
    });
}
#endif

//==============================================================================
class TypefaceCache final : private DeletedAtShutdown
{
public:
    TypefaceCache()
    {
        setSize (10);
    }

    ~TypefaceCache()
    {
        clearSingletonInstance();
    }

    JUCE_DECLARE_SINGLETON (TypefaceCache, false)

    void setSize (const int numToCache)
    {
        const ScopedWriteLock sl (lock);

        faces.clear();
        faces.insertMultiple (-1, CachedFace(), numToCache);
    }

    void clear()
    {
        const ScopedWriteLock sl (lock);

        setSize (faces.size());
        defaultFace = nullptr;
    }

    Typeface::Ptr findTypefaceFor (const Font& font)
    {
        const auto faceName = font.getTypefaceName();
        const auto faceStyle = font.getTypefaceStyle();

        jassert (faceName.isNotEmpty());

        {
            const ScopedReadLock slr (lock);

            for (int i = faces.size(); --i >= 0;)
            {
                CachedFace& face = faces.getReference (i);

                if (face.typefaceName == faceName
                     && face.typefaceStyle == faceStyle
                     && face.typeface != nullptr)
                {
                    face.lastUsageCount = ++counter;
                    return face.typeface;
                }
            }
        }

        const ScopedWriteLock slw (lock);
        int replaceIndex = 0;
        auto bestLastUsageCount = std::numeric_limits<size_t>::max();

        for (int i = faces.size(); --i >= 0;)
        {
            auto lu = faces.getReference (i).lastUsageCount;

            if (bestLastUsageCount > lu)
            {
                bestLastUsageCount = lu;
                replaceIndex = i;
            }
        }

        auto& face = faces.getReference (replaceIndex);
        face.typefaceName = faceName;
        face.typefaceStyle = faceStyle;
        face.lastUsageCount = ++counter;

        if (juce_getTypefaceForFont == nullptr)
            face.typeface = Font::getDefaultTypefaceForFont (font);
        else
            face.typeface = juce_getTypefaceForFont (font);

        jassert (face.typeface != nullptr); // the look and feel must return a typeface!

        if (defaultFace == nullptr && font == Font())
            defaultFace = face.typeface;

        return face.typeface;
    }

    Typeface::Ptr getDefaultFace() const noexcept
    {
        const ScopedReadLock slr (lock);
        return defaultFace;
    }

private:
    struct CachedFace
    {
        CachedFace() noexcept {}

        // Although it seems a bit wacky to store the name here, it's because it may be a
        // placeholder rather than a real one, e.g. "<Sans-Serif>" vs the actual typeface name.
        // Since the typeface itself doesn't know that it may have this alias, the name under
        // which it was fetched needs to be stored separately.
        String typefaceName, typefaceStyle;
        size_t lastUsageCount = 0;
        Typeface::Ptr typeface;
    };

    Typeface::Ptr defaultFace;
    ReadWriteLock lock;
    Array<CachedFace> faces;
    size_t counter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TypefaceCache)
};

JUCE_IMPLEMENT_SINGLETON (TypefaceCache)

void Typeface::setTypefaceCacheSize (int numFontsToCache)
{
    TypefaceCache::getInstance()->setSize (numFontsToCache);
}

void (*clearOpenGLGlyphCache)() = nullptr;

void Typeface::clearTypefaceCache()
{
    TypefaceCache::getInstance()->clear();

    RenderingHelpers::SoftwareRendererSavedState::clearGlyphCache();

    NullCheckedInvocation::invoke (clearOpenGLGlyphCache);
}

//==============================================================================
class Font::SharedFontInternal  : public ReferenceCountedObject
{
public:
    SharedFontInternal() noexcept
        : typeface (TypefaceCache::getInstance()->getDefaultFace()),
          typefaceName (Font::getDefaultSansSerifFontName()),
          typefaceStyle (Font::getDefaultStyle()),
          height (FontValues::defaultFontHeight)
    {
    }

    SharedFontInternal (int styleFlags, float fontHeight) noexcept
        : typefaceName (Font::getDefaultSansSerifFontName()),
          typefaceStyle (FontStyleHelpers::getStyleName (styleFlags)),
          height (fontHeight),
          underline ((styleFlags & underlined) != 0)
    {
        if (styleFlags == plain)
            typeface = TypefaceCache::getInstance()->getDefaultFace();
    }

    SharedFontInternal (const String& name, int styleFlags, float fontHeight) noexcept
        : typefaceName (name),
          typefaceStyle (FontStyleHelpers::getStyleName (styleFlags)),
          height (fontHeight),
          underline ((styleFlags & underlined) != 0)
    {
        if (styleFlags == plain && typefaceName.isEmpty())
            typeface = TypefaceCache::getInstance()->getDefaultFace();
    }

    SharedFontInternal (const String& name, const String& style, float fontHeight) noexcept
        : typefaceName (name), typefaceStyle (style), height (fontHeight)
    {
        if (typefaceName.isEmpty())
            typefaceName = Font::getDefaultSansSerifFontName();
    }

    explicit SharedFontInternal (const Typeface::Ptr& face) noexcept
        : typeface (face),
          typefaceName (face->getName()),
          typefaceStyle (face->getStyle()),
          height (FontValues::defaultFontHeight)
    {
        jassert (typefaceName.isNotEmpty());
    }

    SharedFontInternal (const SharedFontInternal& other) noexcept
        : ReferenceCountedObject(),
          typeface (other.typeface),
          typefaceName (other.typefaceName),
          typefaceStyle (other.typefaceStyle),
          height (other.height),
          horizontalScale (other.horizontalScale),
          kerning (other.kerning),
          ascent (other.ascent),
          underline (other.underline)
    {
    }

    auto tie() const
    {
        return std::tie (height, underline, horizontalScale, kerning, typefaceName, typefaceStyle);
    }

    bool operator== (const SharedFontInternal& other) const noexcept
    {
        return tie() == other.tie();
    }

    bool operator< (const SharedFontInternal& other) const noexcept
    {
        return tie() < other.tie();
    }

    /*  The typeface and ascent data members may be read/set from multiple threads
        simultaneously, e.g. in the case that two Font instances reference the same
        SharedFontInternal and call getTypefacePtr() simultaneously.

        We lock in functions that modify the typeface or ascent in order to
        ensure thread safety.
    */

    Typeface::Ptr getTypefacePtr (const Font& f)
    {
        const ScopedLock lock (mutex);

        if (typeface == nullptr)
        {
            typeface = TypefaceCache::getInstance()->findTypefaceFor (f);
            jassert (typeface != nullptr);
        }

        return typeface;
    }

    HbFont getFontPtr (const Font& f)
    {
        const ScopedLock lock (mutex);

        if (auto ptr = getTypefacePtr (f))
        {
            if (HbFont subFont { hb_font_create_sub_font (ptr->getNativeDetails().getFont()) })
            {
                const auto points = legacyHeightToPoints (ptr, height);

                hb_font_set_ptem (subFont.get(), points);
                hb_font_set_scale (subFont.get(), HbScale::juceToHb (points * horizontalScale), HbScale::juceToHb (points));

               #if JUCE_MAC || JUCE_IOS
                overrideCTFontAdvances (subFont.get(), hb_coretext_font_get_ct_font (subFont.get()));
               #endif

                return subFont;
            }
        }

        return {};
    }

    void resetTypeface()
    {
        const ScopedLock lock (mutex);
        typeface = nullptr;
    }

    float getAscent (const Font& f)
    {
        const ScopedLock lock (mutex);

        if (approximatelyEqual (ascent, 0.0f))
            ascent = getTypefacePtr (f)->getAscent();

        return height * ascent;
    }

    /*  We do not need to lock in these functions, as it's guaranteed
        that these data members can only change if there is a single Font
        instance referencing the shared state.
    */

    StringArray getFallbackFamilies() const { return fallbacks; }
    String getTypefaceName() const          { return typefaceName; }
    String getTypefaceStyle() const         { return typefaceStyle; }
    float getHeight() const                 { return height; }
    float getHorizontalScale() const        { return horizontalScale; }
    float getKerning() const                { return kerning; }
    bool getUnderline() const               { return underline; }
    bool getFallbackEnabled() const         { return fallback; }

    /*  This shared state may be shared between two or more Font instances that are being
        read/modified from multiple threads.
        Before modifying a shared instance you *must* call dupeInternalIfShared to
        ensure that only one Font instance is pointing to the SharedFontInternal instance
        during the modification.
    */

    void setTypeface (Typeface::Ptr newTypeface)
    {
        jassert (getReferenceCount() == 1);
        typeface = newTypeface;

        if (newTypeface != nullptr)
        {
            typefaceName = typeface->getName();
            typefaceStyle = typeface->getStyle();
        }
    }

    void setTypefaceName (String x)
    {
        jassert (getReferenceCount() == 1);
        typefaceName = std::move (x);
    }

    void setTypefaceStyle (String x)
    {
        jassert (getReferenceCount() == 1);
        typefaceStyle = std::move (x);
    }

    void setHeight (float x)
    {
        jassert (getReferenceCount() == 1);
        height = x;
    }

    void setHorizontalScale (float x)
    {
        jassert (getReferenceCount() == 1);
        horizontalScale = x;
    }

    void setKerning (float x)
    {
        jassert (getReferenceCount() == 1);
        kerning = x;
    }

    void setAscent (float x)
    {
        jassert (getReferenceCount() == 1);
        ascent = x;
    }

    void setUnderline (bool x)
    {
        jassert (getReferenceCount() == 1);
        underline = x;
    }

    void setFallbackFamilies (const StringArray& x)
    {
        jassert (getReferenceCount() == 1);
        fallbacks = x;
    }

    void setFallback (bool x)
    {
        jassert (getReferenceCount() == 1);
        fallback = x;
    }

private:
    static float legacyHeightToPoints (Typeface::Ptr p, float h)
    {
        return h * p->getNativeDetails().getLegacyMetrics().getHeightToPointsFactor();
    }

    Typeface::Ptr typeface;
    StringArray fallbacks;
    String typefaceName, typefaceStyle;
    float height = 0.0f, horizontalScale = 1.0f, kerning = 0.0f, ascent = 0.0f;
    bool underline = false;
    bool fallback = true;

    CriticalSection mutex;
};

//==============================================================================
Font::Font()                                : font (new SharedFontInternal()) {}
Font::Font (const Typeface::Ptr& typeface)  : font (new SharedFontInternal (typeface)) {}
Font::Font (const Font& other) noexcept     : font (other.font) {}

Font::Font (float fontHeight, int styleFlags)
    : font (new SharedFontInternal (styleFlags, FontValues::limitFontHeight (fontHeight)))
{
}

Font::Font (const String& typefaceName, float fontHeight, int styleFlags)
    : font (new SharedFontInternal (typefaceName, styleFlags, FontValues::limitFontHeight (fontHeight)))
{
}

Font::Font (const String& typefaceName, const String& typefaceStyle, float fontHeight)
    : font (new SharedFontInternal (typefaceName, typefaceStyle, FontValues::limitFontHeight (fontHeight)))
{
}

Font& Font::operator= (const Font& other) noexcept
{
    font = other.font;
    return *this;
}

Font::Font (Font&& other) noexcept
    : font (std::move (other.font))
{
}

Font& Font::operator= (Font&& other) noexcept
{
    font = std::move (other.font);
    return *this;
}

Font::~Font() noexcept = default;

bool Font::operator== (const Font& other) const noexcept
{
    return font == other.font
            || *font == *other.font;
}

bool Font::operator!= (const Font& other) const noexcept
{
    return ! operator== (other);
}

bool Font::compare (const Font& a, const Font& b) noexcept
{
    return *a.font < *b.font;
}

void Font::dupeInternalIfShared()
{
    if (font->getReferenceCount() > 1)
        font = *new SharedFontInternal (*font);
}

//==============================================================================
struct FontPlaceholderNames
{
    String sans    { "<Sans-Serif>" },
           serif   { "<Serif>" },
           mono    { "<Monospaced>" },
           regular { "<Regular>" };
};

static const FontPlaceholderNames& getFontPlaceholderNames()
{
    static FontPlaceholderNames names;
    return names;
}

#if JUCE_MSVC
// This is a workaround for the lack of thread-safety in MSVC's handling of function-local
// statics - if multiple threads all try to create the first Font object at the same time,
// it can cause a race-condition in creating these placeholder strings.
struct FontNamePreloader { FontNamePreloader() { getFontPlaceholderNames(); } };
static FontNamePreloader fnp;
#endif

const String& Font::getDefaultSansSerifFontName()       { return getFontPlaceholderNames().sans; }
const String& Font::getDefaultSerifFontName()           { return getFontPlaceholderNames().serif; }
const String& Font::getDefaultMonospacedFontName()      { return getFontPlaceholderNames().mono; }
const String& Font::getDefaultStyle()                   { return getFontPlaceholderNames().regular; }

String Font::getTypefaceName() const noexcept           { return font->getTypefaceName(); }
String Font::getTypefaceStyle() const noexcept          { return font->getTypefaceStyle(); }

void Font::setTypefaceName (const String& faceName)
{
    if (faceName != font->getTypefaceName())
    {
        jassert (faceName.isNotEmpty());

        dupeInternalIfShared();
        font->setTypefaceName (faceName);
        font->setTypeface (nullptr);
        font->setAscent (0);
    }
}

void Font::setTypefaceStyle (const String& typefaceStyle)
{
    if (typefaceStyle != font->getTypefaceStyle())
    {
        dupeInternalIfShared();
        font->setTypefaceStyle (typefaceStyle);
        font->setTypeface (nullptr);
        font->setAscent (0);
    }
}

Font Font::withTypefaceStyle (const String& newStyle) const
{
    Font f (*this);
    f.setTypefaceStyle (newStyle);
    return f;
}

StringArray Font::getAvailableStyles() const
{
    return findAllTypefaceStyles (getTypefacePtr()->getName());
}

void Font::setPreferredFallbackFamilies (const StringArray& fallbacks)
{
    if (getPreferredFallbackFamilies() != fallbacks)
    {
        dupeInternalIfShared();
        font->setFallbackFamilies (fallbacks);
    }
}

StringArray Font::getPreferredFallbackFamilies() const
{
    return font->getFallbackFamilies();
}

void Font::setFallbackEnabled (bool enabled)
{
    if (getFallbackEnabled() != enabled)
    {
        dupeInternalIfShared();
        font->setFallback (enabled);
    }
}

bool Font::getFallbackEnabled() const
{
    return font->getFallbackEnabled();
}

Typeface::Ptr Font::getTypefacePtr() const
{
    return font->getTypefacePtr (*this);
}

//==============================================================================
Font Font::withHeight (const float newHeight) const
{
    Font f (*this);
    f.setHeight (newHeight);
    return f;
}

float Font::getHeightToPointsFactor() const
{
    return getTypefacePtr()->getHeightToPointsFactor();
}

Font Font::withPointHeight (float heightInPoints) const
{
    Font f (*this);
    f.setHeight (heightInPoints / getHeightToPointsFactor());
    return f;
}

void Font::setHeight (float newHeight)
{
    newHeight = FontValues::limitFontHeight (newHeight);

    if (! approximatelyEqual (font->getHeight(), newHeight))
    {
        dupeInternalIfShared();
        font->setHeight (newHeight);
        font->resetTypeface();
    }
}

void Font::setHeightWithoutChangingWidth (float newHeight)
{
    newHeight = FontValues::limitFontHeight (newHeight);

    if (! approximatelyEqual (font->getHeight(), newHeight))
    {
        dupeInternalIfShared();
        font->setHorizontalScale (font->getHorizontalScale() * (font->getHeight() / newHeight));
        font->setHeight (newHeight);
        font->resetTypeface();
    }
}

int Font::getStyleFlags() const noexcept
{
    int styleFlags = font->getUnderline() ? underlined : plain;

    if (isBold())    styleFlags |= bold;
    if (isItalic())  styleFlags |= italic;

    return styleFlags;
}

Font Font::withStyle (const int newFlags) const
{
    Font f (*this);
    f.setStyleFlags (newFlags);
    return f;
}

void Font::setStyleFlags (const int newFlags)
{
    if (getStyleFlags() != newFlags)
    {
        dupeInternalIfShared();
        font->setTypeface (nullptr);
        font->setTypefaceStyle (FontStyleHelpers::getStyleName (newFlags));
        font->setUnderline ((newFlags & underlined) != 0);
        font->setAscent (0);
    }
}

void Font::setSizeAndStyle (float newHeight,
                            const int newStyleFlags,
                            const float newHorizontalScale,
                            const float newKerningAmount)
{
    newHeight = FontValues::limitFontHeight (newHeight);

    if (! approximatelyEqual (font->getHeight(), newHeight)
         || ! approximatelyEqual (font->getHorizontalScale(), newHorizontalScale)
         || ! approximatelyEqual (font->getKerning(), newKerningAmount))
    {
        dupeInternalIfShared();
        font->setHeight (newHeight);
        font->setHorizontalScale (newHorizontalScale);
        font->setKerning (newKerningAmount);
        font->resetTypeface();
    }

    setStyleFlags (newStyleFlags);
}

void Font::setSizeAndStyle (float newHeight,
                            const String& newStyle,
                            const float newHorizontalScale,
                            const float newKerningAmount)
{
    newHeight = FontValues::limitFontHeight (newHeight);

    if (! approximatelyEqual (font->getHeight(), newHeight)
         || ! approximatelyEqual (font->getHorizontalScale(), newHorizontalScale)
         || ! approximatelyEqual (font->getKerning(), newKerningAmount))
    {
        dupeInternalIfShared();
        font->setHeight (newHeight);
        font->setHorizontalScale (newHorizontalScale);
        font->setKerning (newKerningAmount);
        font->resetTypeface();
    }

    setTypefaceStyle (newStyle);
}

Font Font::withHorizontalScale (const float newHorizontalScale) const
{
    Font f (*this);
    f.setHorizontalScale (newHorizontalScale);
    return f;
}

void Font::setHorizontalScale (const float scaleFactor)
{
    dupeInternalIfShared();
    font->setHorizontalScale (scaleFactor);
    font->resetTypeface();
}

float Font::getHorizontalScale() const noexcept
{
    return font->getHorizontalScale();
}

float Font::getExtraKerningFactor() const noexcept
{
    return font->getKerning();
}

Font Font::withExtraKerningFactor (const float extraKerning) const
{
    Font f (*this);
    f.setExtraKerningFactor (extraKerning);
    return f;
}

void Font::setExtraKerningFactor (const float extraKerning)
{
    dupeInternalIfShared();
    font->setKerning (extraKerning);
    font->resetTypeface();
}

Font Font::boldened() const                 { return withStyle (getStyleFlags() | bold); }
Font Font::italicised() const               { return withStyle (getStyleFlags() | italic); }

bool Font::isBold() const noexcept          { return FontStyleHelpers::isBold   (font->getTypefaceStyle()); }
bool Font::isItalic() const noexcept        { return FontStyleHelpers::isItalic (font->getTypefaceStyle()); }
bool Font::isUnderlined() const noexcept    { return font->getUnderline(); }

void Font::setBold (const bool shouldBeBold)
{
    auto flags = getStyleFlags();
    setStyleFlags (shouldBeBold ? (flags | bold)
                                : (flags & ~bold));
}

void Font::setItalic (const bool shouldBeItalic)
{
    auto flags = getStyleFlags();
    setStyleFlags (shouldBeItalic ? (flags | italic)
                                  : (flags & ~italic));
}

void Font::setUnderline (const bool shouldBeUnderlined)
{
    dupeInternalIfShared();
    font->setUnderline (shouldBeUnderlined);
    font->resetTypeface();
}

float Font::getAscent() const
{
    return font->getAscent (*this);
}

float Font::getHeight() const noexcept      { return font->getHeight(); }
float Font::getDescent() const              { return font->getHeight() - getAscent(); }

float Font::getHeightInPoints() const       { return getHeight()  * getHeightToPointsFactor(); }
float Font::getAscentInPoints() const       { return getAscent()  * getHeightToPointsFactor(); }
float Font::getDescentInPoints() const      { return getDescent() * getHeightToPointsFactor(); }

int Font::getStringWidth (const String& text) const
{
    return (int) std::ceil (getStringWidthFloat (text));
}

float Font::getStringWidthFloat (const String& text) const
{
    auto w = getTypefacePtr()->getStringWidth (text);

    if (! approximatelyEqual (font->getKerning(), 0.0f))
        w += font->getKerning() * (float) text.length();

    return w * font->getHeight() * font->getHorizontalScale();
}

void Font::getGlyphPositions (const String& text, Array<int>& glyphs, Array<float>& xOffsets) const
{
    getTypefacePtr()->getGlyphPositions (text, glyphs, xOffsets);

    if (auto num = xOffsets.size())
    {
        auto scale = font->getHeight() * font->getHorizontalScale();
        auto* x = xOffsets.getRawDataPointer();

        if (! approximatelyEqual (font->getKerning(), 0.0f))
        {
            for (int i = 0; i < num; ++i)
                x[i] = (x[i] + (float) i * font->getKerning()) * scale;
        }
        else
        {
            for (int i = 0; i < num; ++i)
                x[i] *= scale;
        }
    }
}

void Font::findFonts (Array<Font>& destArray)
{
    for (auto& name : findAllTypefaceNames())
    {
        auto styles = findAllTypefaceStyles (name);

        String style ("Regular");

        if (! styles.contains (style, true))
            style = styles[0];

        destArray.add (Font (name, style, FontValues::defaultFontHeight));
    }
}

static bool characterNotRendered (uint32_t c)
{
    constexpr uint32_t points[]
    {
        // Control points
        0x0000, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x001A, 0x001B, 0x0085,

        // BIDI control points
        0x061C, 0x200E, 0x200F, 0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069
    };

    return std::find (std::begin (points), std::end (points), c) != std::end (points);
}

static bool isFontSuitableForCodepoint (const Font& font, juce_wchar c)
{
    const auto& hbFont = font.getNativeDetails().font;
    hb_codepoint_t glyph{};

    return characterNotRendered ((uint32_t) c)
           || hb_font_get_nominal_glyph (hbFont.get(), (hb_codepoint_t) c, &glyph);
}

static bool isFontSuitableForText (const Font& font, const String& str)
{
    for (const auto c : str)
        if (! isFontSuitableForCodepoint (font, c))
            return false;

    return true;
}

Font Font::findSuitableFontForText (const String& text, const String& language) const
{
    if (! getFallbackEnabled() || isFontSuitableForText (*this, text))
        return *this;

    for (const auto& fallback : getPreferredFallbackFamilies())
    {
        auto copy = *this;
        copy.setTypefaceName (fallback);

        if (isFontSuitableForText (copy, text))
            return copy;
    }

    if (auto current = getTypefacePtr())
    {
        if (auto suggested = current->createSystemFallback (text, language))
        {
            auto copy = *this;

            if (copy.getTypefacePtr() != suggested)
            {
                copy.dupeInternalIfShared();
                copy.font->setTypeface (suggested);
            }

            return copy;
        }
    }

    return *this;
}

//==============================================================================
String Font::toString() const
{
    String s;

    if (getTypefaceName() != getDefaultSansSerifFontName())
        s << getTypefaceName() << "; ";

    s << String (getHeight(), 1);

    if (getTypefaceStyle() != getDefaultStyle())
        s << ' ' << getTypefaceStyle();

    return s;
}

Font Font::fromString (const String& fontDescription)
{
    const int separator = fontDescription.indexOfChar (';');
    String name;

    if (separator > 0)
        name = fontDescription.substring (0, separator).trim();

    if (name.isEmpty())
        name = getDefaultSansSerifFontName();

    String sizeAndStyle (fontDescription.substring (separator + 1).trimStart());

    float height = sizeAndStyle.getFloatValue();
    if (height <= 0)
        height = 10.0f;

    const String style (sizeAndStyle.fromFirstOccurrenceOf (" ", false, false));

    return Font (name, style, height);
}

Font::Native Font::getNativeDetails() const
{
    return { font->getFontPtr (*this) };
}

} // namespace juce
