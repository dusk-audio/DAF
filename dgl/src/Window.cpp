/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2025 Filipe Coelho <falktx@falktx.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with
 * or without fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "WindowPrivateData.hpp"
#include "../TopLevelWidget.hpp"

#include "pugl.hpp"

#include <cstdio>

START_NAMESPACE_DGL

// -----------------------------------------------------------------------
// ScopedGraphicsContext

Window::ScopedGraphicsContext::ScopedGraphicsContext(Window& win)
    : window(win),
      ppData(nullptr),
      active(false),
      reenter(false)
{
    PuglView* const view = window.pData->view;

    if (view == nullptr)
        return;

    // Already current, as for a widget deleted during a plugin UI's teardown or a scope opened inside
    // another: use it, and leave it current when done, since whoever entered it is still using it.
    if (puglBackendIsCurrent(view, false))
    {
        window.pData->createContextIfNeeded();
        return;
    }

    active = puglBackendEnter(view);

    if (active)
        window.pData->createContextIfNeeded();
}

Window::ScopedGraphicsContext::ScopedGraphicsContext(Window& win, Window& transientWin)
    : window(win),
      ppData(transientWin.pData),
      active(false),
      reenter(window.pData->view != nullptr)
{
    if (reenter)
    {
        puglBackendLeave(ppData->view);
        active = puglBackendEnter(window.pData->view);
        if (active)
            window.pData->createContextIfNeeded();
    }
}

Window::ScopedGraphicsContext::~ScopedGraphicsContext()
{
    done();
}

void Window::ScopedGraphicsContext::done()
{
    if (active)
    {
        puglBackendLeave(window.pData->view);
        active = false;
    }

    if (reenter)
    {
        reenter = false;
        DAF_SAFE_ASSERT_RETURN(ppData != nullptr,);

        puglBackendEnter(ppData->view);
    }
}

void Window::ScopedGraphicsContext::reinit()
{
    DAF_SAFE_ASSERT_RETURN(!active,);
    DAF_SAFE_ASSERT_RETURN(!reenter,);
    DAF_SAFE_ASSERT_RETURN(ppData != nullptr,);

    reenter = true;
    puglBackendLeave(ppData->view);
    active = puglBackendEnter(window.pData->view);
}

// -----------------------------------------------------------------------
// Window

Window::Window(Application& app)
    : pData(new PrivateData(app, this))
{
    pData->initPost();
}

Window::Window(Application& app, Window& transientParentWindow)
    : pData(new PrivateData(app, this, transientParentWindow.pData))
{
    pData->initPost();
}

Window::Window(Application& app,
               const uintptr_t parentWindowHandle,
               const double scaleFactor,
               const bool resizable)
    : pData(new PrivateData(app, this, parentWindowHandle, scaleFactor, resizable))
{
    pData->initPost();
}

Window::Window(Application& app,
               const uintptr_t parentWindowHandle,
               const uint width,
               const uint height,
               const double scaleFactor,
               const bool resizable)
    : pData(new PrivateData(app, this, parentWindowHandle, width, height, scaleFactor, resizable, false, false))
{
    pData->initPost();
}

Window::Window(Application& app,
               const uintptr_t parentWindowHandle,
               const uint width,
               const uint height,
               const double scaleFactor,
               const bool resizable,
               const bool usesScheduledRepaints,
               const bool usesSizeRequest,
               const bool doPostInit)
    : pData(new PrivateData(app, this, parentWindowHandle, width, height, scaleFactor, resizable,
                            usesScheduledRepaints, usesSizeRequest))
{
    if (doPostInit)
        pData->initPost();
}

Window::~Window()
{
    delete pData;
}

bool Window::isEmbed() const noexcept
{
    return pData->isEmbed;
}

bool Window::isVisible() const noexcept
{
    return pData->isVisible;
}

void Window::setVisible(const bool visible)
{
    if (visible)
        pData->show();
    else
        pData->hide();
}

void Window::show()
{
    pData->show();
}

void Window::hide()
{
    pData->hide();
}

void Window::close()
{
    pData->close();
}

bool Window::isResizable() const noexcept
{
    return pData->view != nullptr
        && puglGetViewHint(pData->view, PUGL_RESIZABLE) == PUGL_TRUE;
}

void Window::setResizable(const bool resizable)
{
    pData->setResizable(resizable);
}

int Window::getOffsetX() const noexcept
{
    DAF_SAFE_ASSERT_RETURN(pData->view != nullptr, 0);

    return puglGetPositionHint(pData->view, PUGL_CURRENT_POSITION).x;
}

int Window::getOffsetY() const noexcept
{
    DAF_SAFE_ASSERT_RETURN(pData->view != nullptr, 0);

    return puglGetPositionHint(pData->view, PUGL_CURRENT_POSITION).y;
}

Point<int> Window::getOffset() const noexcept
{
    DAF_SAFE_ASSERT_RETURN(pData->view != nullptr, Point<int>());

    const PuglPoint pos = puglGetPositionHint(pData->view, PUGL_CURRENT_POSITION);
    return Point<int>(pos.x, pos.y);
}

void Window::setOffsetX(const int x)
{
    setOffset(x, getOffsetY());
}

void Window::setOffsetY(const int y)
{
    setOffset(getOffsetX(), y);
}

void Window::setOffset(const int x, const int y)
{
    // do not call this for embed windows!
    DAF_SAFE_ASSERT_RETURN(!pData->isEmbed,);

    if (pData->view != nullptr)
        puglSetPositionHint(pData->view, PUGL_CURRENT_POSITION, x, y);
}

void Window::setOffset(const Point<int>& offset)
{
    setOffset(offset.getX(), offset.getY());
}

bool Window::setEmbeddedOffset(const int x, const int y)
{
    DAF_SAFE_ASSERT_RETURN(pData->isEmbed, false);
    DAF_SAFE_ASSERT_RETURN(pData->view != nullptr, false);

    return puglSetPositionHint(pData->view, PUGL_CURRENT_POSITION, x, y) == PUGL_SUCCESS;
}

uint Window::getWidth() const noexcept
{
    DAF_SAFE_ASSERT_RETURN(pData->view != nullptr, 0);

    const PuglSpan width = puglGetSizeHint(pData->view, PUGL_CURRENT_SIZE).width;
    DAF_SAFE_ASSERT(width > 0);
    return width;
}

uint Window::getHeight() const noexcept
{
    DAF_SAFE_ASSERT_RETURN(pData->view != nullptr, 0);

    const PuglSpan height = puglGetSizeHint(pData->view, PUGL_CURRENT_SIZE).height;
    DAF_SAFE_ASSERT(height > 0);
    return height;
}

Size<uint> Window::getSize() const noexcept
{
    DAF_SAFE_ASSERT_RETURN(pData->view != nullptr, Size<uint>());

    const PuglArea size = puglGetSizeHint(pData->view, PUGL_CURRENT_SIZE);
    DAF_SAFE_ASSERT(size.width > 0);
    DAF_SAFE_ASSERT(size.height > 0);
    return Size<uint>(size.width, size.height);
}

void Window::setWidth(const uint width)
{
    setSize(width, getHeight());
}

void Window::setHeight(const uint height)
{
    setSize(getWidth(), height);
}

void Window::setSize(uint width, uint height)
{
    DAF_SAFE_ASSERT_UINT2_RETURN(width > 1 && height > 1, width, height,);

    if (pData->isEmbed)
    {
        const double scaleFactor = pData->scaleFactor;
        uint minWidth = pData->minWidth;
        uint minHeight = pData->minHeight;

        if (pData->autoScaling && d_isNotEqual(scaleFactor, 1.0))
        {
            minWidth = d_roundToUnsignedInt(minWidth * scaleFactor);
            minHeight = d_roundToUnsignedInt(minHeight * scaleFactor);
        }

        // handle geometry constraints here
        if (width < minWidth)
            width = minWidth;

        if (height < minHeight)
            height = minHeight;

        if (pData->keepAspectRatio)
        {
            const double ratio = static_cast<double>(pData->minWidth)
                               / static_cast<double>(pData->minHeight);
            const double reqRatio = static_cast<double>(width)
                                  / static_cast<double>(height);

            if (d_isNotEqual(ratio, reqRatio))
            {
                // fix width
                if (reqRatio > ratio)
                    width = d_roundToUnsignedInt(height * ratio);
                // fix height
                else
                    height = d_roundToUnsignedInt(static_cast<double>(width) / ratio);
            }
        }
    }

    if (pData->usesSizeRequest)
    {
        DAF_SAFE_ASSERT_RETURN(pData->topLevelWidgets.size() != 0,);

        TopLevelWidget* const topLevelWidget = pData->topLevelWidgets.front();
        DAF_SAFE_ASSERT_RETURN(topLevelWidget != nullptr,);

        topLevelWidget->requestSizeChange(width, height);
    }
    else if (pData->view != nullptr)
    {
        puglSetSizeAndDefault(pData->view, width, height);

        // there are no resize events for closed windows, so short-circuit the top-level widgets here
        if (pData->isClosed)
        {
            for (std::list<TopLevelWidget*>::iterator it = pData->topLevelWidgets.begin(),
                                                      end = pData->topLevelWidgets.end(); it != end; ++it)
            {
                ((Widget*)*it)->setSize(width, height);
            }
        }
    }
}

void Window::setSize(const Size<uint>& size)
{
    setSize(size.getWidth(), size.getHeight());
}

const char* Window::getTitle() const noexcept
{
    return pData->view != nullptr ? puglGetViewString(pData->view, PUGL_WINDOW_TITLE) : "";
}

void Window::setTitle(const char* const title)
{
    if (pData->view != nullptr)
        puglSetViewString(pData->view, PUGL_WINDOW_TITLE, title);
}

bool Window::isIgnoringKeyRepeat() const noexcept
{
    return pData->view != nullptr
        && puglGetViewHint(pData->view, PUGL_IGNORE_KEY_REPEAT) == PUGL_TRUE;
}

void Window::setIgnoringKeyRepeat(const bool ignore) noexcept
{
    if (pData->view != nullptr)
        puglSetViewHint(pData->view, PUGL_IGNORE_KEY_REPEAT, ignore);
}

const void* Window::getClipboard(size_t& dataSize)
{
    return pData->getClipboard(dataSize);
}

bool Window::setClipboard(const char* const mimeType, const void* const data, const size_t dataSize)
{
    return pData->view != nullptr
        && puglSetClipboard(pData->view, mimeType != nullptr ? mimeType : "text/plain", data, dataSize) == PUGL_SUCCESS;
}

bool Window::setCursor(const MouseCursor cursor)
{
    return pData->view != nullptr
        && puglSetCursor(pData->view, static_cast<PuglCursor>(cursor)) == PUGL_SUCCESS;
}

bool Window::addIdleCallback(IdleCallback* const callback, const uint timerFrequencyInMs)
{
    DAF_SAFE_ASSERT_RETURN(callback != nullptr, false)

    return pData->addIdleCallback(callback, timerFrequencyInMs);
}

bool Window::removeIdleCallback(IdleCallback* const callback)
{
    DAF_SAFE_ASSERT_RETURN(callback != nullptr, false)

    return pData->removeIdleCallback(callback);
}

Application& Window::getApp() const noexcept
{
    return pData->app;
}

#ifndef DAF_TEST_WINDOW_CPP
const GraphicsContext& Window::getGraphicsContext() const noexcept
{
    return pData->getGraphicsContext();
}
#endif

uintptr_t Window::getNativeWindowHandle() const noexcept
{
    return pData->view != nullptr ? puglGetNativeView(pData->view) : 0;
}

const char* Window::getPortalParentHandle() const noexcept
{
    char* const handle = pData->portalParentHandle;
    handle[0] = '\0';

    if (pData->view == nullptr)
        return handle;

   #if defined(DGL_USING_X11)
    if (const uintptr_t win = puglGetNativeView(pData->view))
        std::snprintf(handle, sizeof(pData->portalParentHandle), "x11:0x%llx",
                      static_cast<unsigned long long>(win));
   #elif defined(DGL_USING_WAYLAND)
    if (const char* const exported = puglWaylandGetExportedHandle(pData->view))
        std::snprintf(handle, sizeof(pData->portalParentHandle), "wayland:%s", exported);
   #endif

    return handle;
}

double Window::getScaleFactor() const noexcept
{
    return pData->scaleFactor;
}

void Window::focus()
{
    pData->focus();
}

#ifdef DGL_USE_FILE_BROWSER
bool Window::openFileBrowser(const FileBrowserOptions& options)
{
    return pData->openFileBrowser(options);
}
#endif

#ifdef DGL_USE_WEB_VIEW
bool Window::createWebView(const char* const url, const DGL_NAMESPACE::WebViewOptions& options)
{
    return pData->createWebView(url, options);
}

void Window::evaluateJS(const char* const js)
{
    DAF_SAFE_ASSERT_RETURN(pData->webViewHandle != nullptr,);

    webViewEvaluateJS(pData->webViewHandle, js);
}
#endif

void Window::repaint() noexcept
{
    if (pData->view == nullptr)
        return;

    if (pData->usesScheduledRepaints)
        pData->appData->needsRepaint = true;

    puglObscureView(pData->view);
}

void Window::repaint(const Rectangle<uint>& rect) noexcept
{
    if (pData->view == nullptr)
        return;

    if (pData->usesScheduledRepaints)
        pData->appData->needsRepaint = true;

    int x = static_cast<int>(rect.getX());
    int y = static_cast<int>(rect.getY());
    uint width = rect.getWidth();
    uint height = rect.getHeight();

    if (pData->autoScaling)
    {
        const double autoScaleFactor = pData->autoScaleFactor;

        x = d_roundToIntPositive(x * autoScaleFactor);
        y = d_roundToIntPositive(y * autoScaleFactor);
        width = d_roundToUnsignedInt(width * autoScaleFactor);
        height = d_roundToUnsignedInt(height * autoScaleFactor);
    }

    puglObscureRegion(pData->view, x, y, width, height);
}

void Window::renderToPicture(const char* const filename)
{
    pData->filenameToRenderInto = strdup(filename);
}

void Window::runAsModal(bool blockWait)
{
    pData->runAsModal(blockWait);
}

Size<uint> Window::getGeometryConstraints(bool& keepAspectRatio)
{
    keepAspectRatio = pData->keepAspectRatio;
    return Size<uint>(pData->minWidth, pData->minHeight);
}

void Window::setGeometryConstraints(uint minimumWidth,
                                    uint minimumHeight,
                                    const bool keepAspectRatio,
                                    const bool automaticallyScale,
                                    bool resizeNowIfAutoScaling)
{
    DAF_SAFE_ASSERT_RETURN(minimumWidth > 0,);
    DAF_SAFE_ASSERT_RETURN(minimumHeight > 0,);

    /* Only the transition into auto-scaling mode sizes the window from the constraints. A window
       that is already auto-scaling keeps whatever size it has, so a second call cannot pull a
       user-resized window back to its minimum. */
    if (resizeNowIfAutoScaling && automaticallyScale && pData->autoScaling == automaticallyScale)
        resizeNowIfAutoScaling = false;

    pData->minWidth = minimumWidth;
    pData->minHeight = minimumHeight;
    pData->autoScaling = automaticallyScale;
    pData->keepAspectRatio = keepAspectRatio;

    if (pData->view == nullptr)
        return;

    const double scaleFactor = pData->scaleFactor;

    if (automaticallyScale && scaleFactor != 1.0)
    {
        minimumWidth = d_roundToUnsignedInt(minimumWidth * scaleFactor);
        minimumHeight = d_roundToUnsignedInt(minimumHeight * scaleFactor);
    }

    puglSetGeometryConstraints(pData->view, minimumWidth, minimumHeight, keepAspectRatio);

    if (scaleFactor != 1.0 && automaticallyScale && resizeNowIfAutoScaling)
    {
        /* Size the window from the already-scaled minimum rather than from its current size times
           the factor. Plugin UIs create their window at the scaled size before this runs -- see
           UI::PrivateData::createNextWindow, which scales the requested size while building the
           window -- so multiplying the current size applied the factor a second time and every
           auto-scaling UI opened at scale-squared size. Taking the target from the constraints is
           the same result for a window still at its unscaled design size, and idempotent for one
           that is already scaled. */
        setSize(minimumWidth, minimumHeight);
    }
}

void Window::setTransientParent(const uintptr_t transientParentWindowHandle)
{
    if (pData->view != nullptr)
        puglSetTransientParent(pData->view, transientParentWindowHandle);
}

std::vector<ClipboardDataOffer> Window::getClipboardDataOfferTypes()
{
    std::vector<ClipboardDataOffer> offerTypes;

    if (pData->view == nullptr)
        return offerTypes;

    if (const uint32_t numTypes = puglGetNumClipboardTypes(pData->view))
    {
        offerTypes.reserve(numTypes);

        for (uint32_t i=0; i<numTypes; ++i)
        {
            const ClipboardDataOffer offer = { i + 1, puglGetClipboardType(pData->view, i) };
            offerTypes.push_back(offer);
        }
    }

    return offerTypes;
}

uint32_t Window::onClipboardDataOffer()
{
    std::vector<ClipboardDataOffer> offers(getClipboardDataOfferTypes());

    for (std::vector<ClipboardDataOffer>::iterator it=offers.begin(), end=offers.end(); it != end;++it)
    {
        const ClipboardDataOffer offer = *it;
        if (std::strcmp(offer.type, "text/plain") == 0)
            return offer.id;
    }

    return 0;
}

bool Window::onClose()
{
    return true;
}

void Window::onFocus(bool, CrossingMode)
{
}

void Window::onReshape(const uint width, const uint height)
{
    if (pData->view != nullptr)
        puglFallbackOnResize(pData->view, width, height);
}

void Window::onScaleFactorChanged(double)
{
}

#ifdef DGL_USE_FILE_BROWSER
void Window::onFileSelected(const char*)
{
}
#endif

// -----------------------------------------------------------------------

END_NAMESPACE_DGL
