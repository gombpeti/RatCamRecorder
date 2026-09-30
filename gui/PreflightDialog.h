#pragma once

// Pre-flight checklist shown when START is pressed.
//
// A recording session is usually not repeatable: the animal has been handled,
// the protocol has begun, and the wrong folder name or a missing camera is only
// discovered afterwards. The three things that have actually gone wrong on this
// rig -- a stale session folder, the wrong duration, and a camera that failed
// to open -- are therefore put in front of the operator and each has to be
// ticked individually. Recording cannot begin until all three are confirmed.
//
// Deliberately not a single "Are you sure?" button: that gets clicked
// reflexively. Three separate acknowledgements of three specific facts do not.

#include <QDialog>
#include <QString>
#include <vector>

class QCheckBox;
class QPushButton;

namespace campy {

class PreflightDialog : public QDialog {
    Q_OBJECT

public:
    struct CameraLine {
        QString name;
        QString serial;
        bool ok = true;
        QString note;      // why it is not ok, when it is not
    };

    PreflightDialog(const QString& folder, int seconds, double fps,
                    const std::vector<CameraLine>& cameras,
                    int expectedCameras, QWidget* parent = nullptr);

private:
    void UpdateStartButton();

    QCheckBox* folderCheck_ = nullptr;
    QCheckBox* durationCheck_ = nullptr;
    QCheckBox* cameraCheck_ = nullptr;
    QPushButton* startButton_ = nullptr;
};

} // namespace campy
