#include "PreflightDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace campy {
namespace {

QLabel* Value(const QString& text) {
    auto* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() + 1.0);
    l->setFont(f);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
    return l;
}

} // namespace

PreflightDialog::PreflightDialog(const QString& folder, int seconds, double fps,
                                 const std::vector<CameraLine>& cameras,
                                 int expectedCameras, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("RatCam Recorder -- confirm before recording");
    setModal(true);
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        "Check each item and tick it. Recording starts only when all three "
        "are confirmed.");
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // --- 1. session folder ---
    {
        auto* box = new QGroupBox("1.  Session folder");
        auto* v = new QVBoxLayout(box);
        v->addWidget(Value(folder));
        folderCheck_ = new QCheckBox("The session folder name is correct");
        v->addWidget(folderCheck_);
        layout->addWidget(box);
    }

    // --- 2. duration ---
    {
        auto* box = new QGroupBox("2.  Duration");
        auto* v = new QVBoxLayout(box);
        const int mins = seconds / 60, secs = seconds % 60;
        // Frames as well as seconds: the recording actually stops on a frame
        // count, and that is the number that ends up in the files.
        v->addWidget(Value(QString("%1 s  (%2 min %3 s)   =  %4 frames at %5 fps")
                               .arg(seconds).arg(mins).arg(secs, 2, 10, QChar('0'))
                               .arg(qRound(seconds * fps)).arg(fps, 0, 'g', 4)));
        durationCheck_ = new QCheckBox("The duration is correct");
        v->addWidget(durationCheck_);
        layout->addWidget(box);
    }

    // --- 3. cameras ---
    {
        auto* box = new QGroupBox("3.  Cameras");
        auto* v = new QVBoxLayout(box);

        const int found = static_cast<int>(cameras.size());
        const bool countOk = (expectedCameras <= 0) || (found == expectedCameras);

        auto* summary = Value(QString("%1 camera(s) detected").arg(found));
        if (!countOk || found == 0)
            summary->setStyleSheet("color: #c0392b;");
        v->addWidget(summary);

        for (const CameraLine& c : cameras) {
            QString line = QString("   %1   %2").arg(c.name, c.serial);
            if (!c.ok) line += "   -- " + c.note;
            auto* l = new QLabel(line);
            if (!c.ok) l->setStyleSheet("color: #c0392b;");
            v->addWidget(l);
        }

        if (found == 0) {
            auto* warn = new QLabel("No cameras were found. Nothing would be recorded.");
            warn->setStyleSheet("color: #c0392b;");
            warn->setWordWrap(true);
            v->addWidget(warn);
        }

        cameraCheck_ = new QCheckBox("All expected cameras are present and OK");
        // Ticking is still the operator's decision even when something looks
        // wrong -- they may knowingly be recording with five cameras -- but it
        // cannot be ticked when there is nothing at all to record.
        cameraCheck_->setEnabled(found > 0);
        v->addWidget(cameraCheck_);
        layout->addWidget(box);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    startButton_ = buttons->addButton("START RECORDING", QDialogButtonBox::AcceptRole);
    startButton_->setDefault(false);
    startButton_->setAutoDefault(false);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (QCheckBox* c : {folderCheck_, durationCheck_, cameraCheck_})
        connect(c, &QCheckBox::toggled, this, &PreflightDialog::UpdateStartButton);

    UpdateStartButton();
}

void PreflightDialog::UpdateStartButton() {
    const bool all = folderCheck_->isChecked() &&
                     durationCheck_->isChecked() &&
                     cameraCheck_->isChecked();
    startButton_->setEnabled(all);
    startButton_->setText(all ? "START RECORDING"
                              : "START RECORDING  (confirm all three)");
}

} // namespace campy
