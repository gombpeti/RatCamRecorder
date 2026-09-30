#pragma once

// Top-level window that hosts the Live View when it is detached.
//
// Deliberately hosts the *same* PreviewGrid instance rather than a second one:
// the grid is reparented in and out. A duplicate would need its own feed from
// the cameras, which is exactly the kind of second path that drifts out of step
// with the first and ends up showing something the recording is not doing.
//
// Built for the two-monitor case: it opens on whichever screen is not the main
// window's, and F11 (or a double-click) toggles full screen.

#include <QWidget>

class QCloseEvent;
class QKeyEvent;
class QMouseEvent;

namespace campy {

class PreviewGrid;

class PreviewWindow : public QWidget {
    Q_OBJECT

public:
    /// Takes ownership of `grid` for as long as it is shown here.
    PreviewWindow(PreviewGrid* grid, QWidget* mainWindow);

    /// Move to a screen other than the main window's, if there is one, and go
    /// full screen. Does nothing beyond full screen on a single-monitor setup.
    void ShowOnSecondScreen();

signals:
    /// Emitted when the window closes, so the caller can re-dock the grid.
    void Closing();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void ToggleFullScreen();

    PreviewGrid* grid_ = nullptr;
    QWidget* mainWindow_ = nullptr;
};

} // namespace campy
