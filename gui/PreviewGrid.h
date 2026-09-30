#pragma once

// Live view of every camera, tiled in one widget.
//
// campy opened one pylon CPylonImageWindow per camera and scattered six popups
// across the desktop. This keeps them inside the application window, which is
// easier to arrange and lets the label (name, fps, loss) live on the image.
//
// The images arrive already downsampled by CameraWorker, so this widget only
// scales and blits. It never talks to pylon and never touches the recording
// path -- if it stopped updating entirely, the recording would be unaffected.

#include <cstdint>
#include <vector>

#include <QImage>
#include <QString>
#include <QWidget>

namespace campy {

class PreviewGrid : public QWidget {
    Q_OBJECT

public:
    explicit PreviewGrid(QWidget* parent = nullptr);

    /// Size the grid. Clears any existing images.
    void SetCameraCount(int n);

    /// Replace one camera's image. `rgb` is packed RGB24, w*h*3 bytes.
    void SetImage(int index, const std::vector<uint8_t>& rgb, int w, int h);

    /// Caption drawn over the image, and whether to flag it red.
    void SetLabel(int index, const QString& text, bool alarm);

    void Clear();

    /// Force a column count, e.g. 3 so six cameras sit 2 rows x 3 columns.
    /// Ignored when the widget is too narrow for cells of a usable size, in
    /// which case the automatic layout takes over rather than producing a row
    /// of slivers. 0 restores fully automatic behaviour.
    void SetPreferredColumns(int cols) { preferredCols_ = cols; update(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Cell {
        QImage image;
        QString label;
        bool alarm = false;
    };
    std::vector<Cell> cells_;
    int preferredCols_ = 0;
};

} // namespace campy
