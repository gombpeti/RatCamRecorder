#include "PreviewWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QVBoxLayout>

#include "PreviewGrid.h"

namespace campy {

PreviewWindow::PreviewWindow(PreviewGrid* grid, QWidget* mainWindow)
    : QWidget(nullptr), grid_(grid), mainWindow_(mainWindow) {
    setWindowTitle("RatCam Recorder -- Live View");
    setWindowFlag(Qt::Window);
    resize(1280, 800);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* hint = new QLabel("F11 or double-click: full screen    "
                            "Esc: leave full screen    "
                            "Close this window to dock the view again");
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet("color: #909090; padding: 3px;");
    layout->addWidget(hint);

    grid_->setParent(this);
    // Six cameras as 2 rows x 3 columns, which is what a wide monitor wants.
    grid_->SetPreferredColumns(3);
    layout->addWidget(grid_, 1);
    grid_->show();
}

void PreviewWindow::ShowOnSecondScreen() {
    QScreen* target = nullptr;
    const QScreen* mine = mainWindow_ && mainWindow_->screen()
                        ? mainWindow_->screen() : QGuiApplication::primaryScreen();

    for (QScreen* s : QGuiApplication::screens()) {
        if (s != mine) { target = s; break; }
    }

    if (target) {
        // Place it on the other screen before going full screen, or the window
        // manager puts it full screen on the screen it currently occupies.
        setGeometry(target->geometry());
        showFullScreen();
    } else {
        // Single monitor: full screen still makes sense, it just covers this one.
        show();
        showFullScreen();
    }
}

void PreviewWindow::ToggleFullScreen() {
    if (isFullScreen()) showNormal();
    else showFullScreen();
}

void PreviewWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F11) { ToggleFullScreen(); return; }
    if (event->key() == Qt::Key_Escape && isFullScreen()) { showNormal(); return; }
    QWidget::keyPressEvent(event);
}

void PreviewWindow::mouseDoubleClickEvent(QMouseEvent*) {
    ToggleFullScreen();
}

void PreviewWindow::closeEvent(QCloseEvent* event) {
    // Let go of the grid before the window is destroyed, so the caller can put
    // it back into the main window rather than it being deleted as a child.
    if (grid_) grid_->setParent(nullptr);
    emit Closing();
    event->accept();
}

} // namespace campy
