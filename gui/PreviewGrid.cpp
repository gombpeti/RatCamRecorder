#include "PreviewGrid.h"

#include <algorithm>
#include <cmath>

#include <QPainter>
#include <QPaintEvent>

namespace campy {
namespace {

/// Rows x cols that renders each 16:10 frame largest, the same rule campy uses
/// in display.py GridLayout(): six cameras land on 3x2 on a widescreen.
void ChooseGrid(int n, int widgetW, int widgetH, int& cols, int& rows) {
    cols = std::max(1, n);
    rows = 1;
    double best = -1.0;
    for (int c = 1; c <= std::max(1, n); ++c) {
        const int r = (n + c - 1) / c;                 // ceiling division
        const double scale = std::min((widgetW / double(c)) / 16.0,
                                      (widgetH / double(r)) / 10.0);
        if (scale > best) { best = scale; cols = c; rows = r; }
    }
}

} // namespace

PreviewGrid::PreviewGrid(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(220);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(28, 28, 30));
    setPalette(pal);
}

void PreviewGrid::SetCameraCount(int n) {
    cells_.assign(std::max(0, n), Cell{});
    update();
}

void PreviewGrid::SetImage(int index, const std::vector<uint8_t>& rgb,
                           int w, int h) {
    if (index < 0 || index >= static_cast<int>(cells_.size())) return;
    if (w <= 0 || h <= 0 ||
        rgb.size() < static_cast<size_t>(w) * h * 3) return;

    // QImage over borrowed memory would dangle once `rgb` goes away, so copy.
    // At 480x300 that is 432 KB per camera per update -- trivial next to the
    // 1.24 GB/s the recording path is handling.
    cells_[index].image =
        QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
    update();
}

void PreviewGrid::SetLabel(int index, const QString& text, bool alarm) {
    if (index < 0 || index >= static_cast<int>(cells_.size())) return;
    cells_[index].label = text;
    cells_[index].alarm = alarm;
    update();
}

void PreviewGrid::Clear() {
    for (Cell& c : cells_) { c.image = QImage(); c.label.clear(); c.alarm = false; }
    update();
}

void PreviewGrid::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 28, 30));

    const int n = static_cast<int>(cells_.size());
    if (n == 0) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect(), Qt::AlignCenter, "No cameras");
        return;
    }

    int cols = 1, rows = 1;
    ChooseGrid(n, width(), height(), cols, rows);

    // An explicit column count wins while the cells stay big enough to be
    // worth looking at; below that the automatic layout is the lesser evil.
    if (preferredCols_ > 0) {
        const int pr = (n + preferredCols_ - 1) / preferredCols_;
        if (width() / preferredCols_ >= 120 && height() / pr >= 80) {
            cols = preferredCols_;
            rows = pr;
        }
    }
    const int cellW = width() / cols;
    const int cellH = height() / rows;

    for (int i = 0; i < n; ++i) {
        const QRect cell((i % cols) * cellW, (i / cols) * cellH, cellW, cellH);
        const QRect inner = cell.adjusted(2, 2, -2, -2);
        const Cell& c = cells_[i];

        if (!c.image.isNull()) {
            // Keep aspect ratio: a stretched preview misleads about the scene.
            const QImage scaled = c.image.scaled(inner.size(), Qt::KeepAspectRatio,
                                                 Qt::FastTransformation);
            const QPoint at(inner.x() + (inner.width() - scaled.width()) / 2,
                            inner.y() + (inner.height() - scaled.height()) / 2);
            p.drawImage(at, scaled);
        } else {
            p.fillRect(inner, QColor(45, 45, 48));
            p.setPen(QColor(120, 120, 120));
            p.drawText(inner, Qt::AlignCenter, "waiting...");
        }

        if (!c.label.isEmpty()) {
            const QRect bar(inner.x(), inner.bottom() - 20, inner.width(), 20);
            p.fillRect(bar, c.alarm ? QColor(150, 0, 0, 200)
                                    : QColor(0, 0, 0, 150));
            p.setPen(Qt::white);
            p.drawText(bar.adjusted(6, 0, -6, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, c.label);
        }

        p.setPen(c.alarm ? QColor(220, 60, 60) : QColor(70, 70, 74));
        p.drawRect(inner);
    }
}

} // namespace campy
