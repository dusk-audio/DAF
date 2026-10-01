/*
 * DAF - Dusk Audio Framework
 * Copyright (C) 2026 Dusk Audio
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

#include "tests.hpp"

#include "dgl/SubWidget.hpp"
#include "dgl/TopLevelWidget.hpp"
#include "dgl/Window.hpp"

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::SubWidget;
    using DGL_NAMESPACE::TopLevelWidget;
    using DGL_NAMESPACE::Window;

    Application app(true);
    Window win(app);

    // a subwidget destroyed after its parent subwidget is detached, not left with a dangling parent
    {
        TopLevelWidget tlw(win);
        SubWidget* const parent = new SubWidget(&tlw);
        SubWidget* const child = new SubWidget(parent);
        DAF_ASSERT_EQUAL(child->getParentWidget(), parent, "child has its parent");

        delete parent;
        DAF_ASSERT_EQUAL(child->getParentWidget(), nullptr, "child is detached when its parent goes first");
        DAF_ASSERT_EQUAL(child->getTopLevelWidget(), &tlw, "child keeps a top-level widget that still exists");

        // these used to dereference the freed parent
        child->toFront();
        child->toBottom();
        delete child;
    }

    // destroying the top-level widget first detaches its whole subtree
    {
        TopLevelWidget* const tlw = new TopLevelWidget(win);
        SubWidget* const child = new SubWidget(tlw);
        SubWidget* const grandchild = new SubWidget(child);
        DAF_ASSERT_EQUAL(grandchild->getTopLevelWidget(), tlw, "grandchild has its top-level widget");

        delete tlw;
        DAF_ASSERT_EQUAL(child->getParentWidget(), nullptr, "child is detached from the top-level widget");
        DAF_ASSERT_EQUAL(child->getTopLevelWidget(), nullptr, "child loses the destroyed top-level widget");
        DAF_ASSERT_EQUAL(grandchild->getParentWidget(), child, "grandchild keeps its living parent");
        DAF_ASSERT_EQUAL(grandchild->getTopLevelWidget(), nullptr, "grandchild loses the destroyed top-level widget");

        // repaint goes through the top-level widget, and must cope with it being gone
        grandchild->repaint();

        delete child;
        DAF_ASSERT_EQUAL(grandchild->getParentWidget(), nullptr, "grandchild is detached in turn");
        delete grandchild;
    }

    // the regular order still works
    {
        TopLevelWidget tlw(win);
        SubWidget* const parent = new SubWidget(&tlw);
        SubWidget* const child = new SubWidget(parent);
        delete child;
        delete parent;
    }

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
