#pragma once

// The window's look (U1 phase 6, decision 0039): one palette and one style
// sheet in the mockup's colours, set on the main window so everything in
// it (docks, the Browser, the Problems drawer, dialogs it parents)
// follows. Light only; a dark theme is out of U1's scope.
//
// The colours are the mockup's: cool greys from near-white panels to
// near-black text, and one blue accent for the current tab, selection and
// focus. Widgets keep their own sizes: the sheet sets colours, borders
// and header padding only, never a tool button's padding (the context
// bar's toolbars measure themselves).

#include <QColor>
#include <QString>

class QWidget;

namespace vs::app::theme {

inline const QColor kText{0x11, 0x18, 0x27};        // near black
inline const QColor kTextMuted{0x4b, 0x55, 0x63};   // captions, headers
inline const QColor kTextFaint{0x6b, 0x72, 0x80};   // hints, details
inline const QColor kBorder{0xe5, 0xe7, 0xeb};      // panel edges
inline const QColor kBorderStrong{0xd1, 0xd5, 0xdb};
inline const QColor kWindow{0xf3, 0xf4, 0xf6};      // behind the panels
inline const QColor kPanel{0xff, 0xff, 0xff};       // panels, lists
inline const QColor kPanelAlt{0xf9, 0xfa, 0xfb};    // headers, alternate rows
inline const QColor kAccent{0x25, 0x63, 0xeb};      // current tab, focus
inline const QColor kSelection{0xdb, 0xea, 0xfe};   // selected rows

// The style sheet, for tests and for anything shown outside the window.
QString style_sheet();
// Sets the palette and the style sheet on `window`.
void apply(QWidget& window);

}  // namespace vs::app::theme
