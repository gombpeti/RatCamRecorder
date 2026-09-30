#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QEventLoop>
#include <QStatusBar>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStyleFactory>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <filesystem>

#include <pylon/PylonIncludes.h>

#include "PreflightDialog.h"
#include "PreviewGrid.h"
#include "PreviewWindow.h"
#include "core/Recorder.h"
#include "trigger/SerialTrigger.h"

namespace campy {
namespace {

const QStringList kHeaders = {"Camera", "Serial", "fps", "Received",
                              "Encoded", "Lost", "Dropped", "Buffer"};

/// USB vendor IDs that identify a trigger board, ported from campy gui.py.
/// Genuine Arduino vendors score highest; the generic USB-serial bridges are
/// what clone boards ship with, so they are likely but not certain.
struct VendorScore { quint16 vid; int score; const char* what; };
const VendorScore kArduinoVids[] = {
    {0x2341, 3, "Arduino"},
    {0x2A03, 3, "Arduino (Genuino)"},
    {0x1B4F, 2, "SparkFun"},
    {0x239A, 2, "Adafruit"},
    {0x1A86, 1, "CH340 bridge (clone board)"},
    {0x0403, 1, "FTDI bridge (clone board)"},
    {0x10C4, 1, "CP210x bridge (clone board)"},
};

/// Product IDs worth naming exactly. The rig runs a Portenta H7, whose native
/// USB CDC means opening the port does NOT reset the board -- unlike an Uno or
/// Mega, so no bootloader settling is needed and no banner is reprinted.
struct ProductName { quint16 vid, pid; const char* what; };
const ProductName kKnownBoards[] = {
    {0x2341, 0x025B, "Arduino Portenta H7"},
    {0x2341, 0x035B, "Arduino Portenta H7 (bootloader)"},
    {0x2341, 0x0266, "Arduino Giga R1"},
};

/// How likely this port is the trigger board. 0 means "probably not".
int ScorePort(const QSerialPortInfo& info, QString& why) {
    if (info.hasVendorIdentifier() && info.hasProductIdentifier()) {
        for (const ProductName& b : kKnownBoards) {
            if (info.vendorIdentifier() == b.vid &&
                info.productIdentifier() == b.pid) { why = b.what; return 4; }
        }
    }
    if (info.hasVendorIdentifier()) {
        for (const VendorScore& v : kArduinoVids) {
            if (info.vendorIdentifier() == v.vid) { why = v.what; return v.score; }
        }
    }
    const QString desc = (info.description() + " " + info.manufacturer()).toLower();
    if (desc.contains("arduino")) { why = "named Arduino"; return 2; }
    why = info.description().isEmpty() ? "serial port" : info.description();
    return 0;
}

/// Dark palette. Qt's Fusion style honours the palette on every platform,
/// unlike the native Windows style which ignores most of it.
QPalette DarkPalette() {
    QPalette p;
    const QColor base(30, 31, 34), alt(38, 39, 43), text(220, 221, 224);
    p.setColor(QPalette::Window, base);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, QColor(24, 25, 28));
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, alt);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, QColor(52, 120, 200));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(94, 158, 214));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(130, 131, 134));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(130, 131, 134));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(130, 131, 134));
    return p;
}

/// The optional top-view camera. It shares the trigger but captures one frame
/// per two pulses, and is a different model at a different resolution and
/// pixel format, so it needs its own .pfs and its own rate.
const char* kTopViewName   = "CAM0";
const char* kTopViewModel  = "a2A3536";
const char* kTopViewSerial = "41975154";

bool IsTopView(const QString& name, const QString& model, const QString& serial) {
    return name.compare(kTopViewName, Qt::CaseInsensitive) == 0
        || serial == kTopViewSerial
        || model.startsWith(kTopViewModel, Qt::CaseInsensitive);
}

/// Where the rig keeps its .pfs files.
QString ConfigsDir() {
    for (const QString& guess : {QStringLiteral("E:/campy/campy/configs"),
                                 QDir::currentPath() + "/../configs",
                                 QDir::currentPath() + "/configs"}) {
        if (QDir(guess).exists()) return guess;
    }
    return QString();
}

/// Longest shared leading run of characters, case-insensitively.
int SharedPrefix(const QString& a, const QString& b) {
    int n = 0;
    while (n < a.size() && n < b.size() &&
           a[n].toLower() == b[n].toLower()) ++n;
    return n;
}

/// Pick the .pfs whose filename best matches the detected camera model.
///
/// Choosing by hand is error prone: this rig has a2A1920 cameras and a
/// leftover set of a2A3536 settings files, and picking the wrong one fails
/// every camera with a wall of "node not found". The model name is in both the
/// camera and the filename, so the match can simply be made.
QString BestPfsForModel(const QString& model) {
    const QString dir = ConfigsDir();
    if (dir.isEmpty() || model.isEmpty()) return QString();

    QString best;
    int bestScore = 0;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"*.pfs"}, QDir::Files)) {
        const int score = SharedPrefix(fi.fileName(), model);
        if (score > bestScore) { bestScore = score; best = fi.absoluteFilePath(); }
    }
    // Require a real match ("a2A1920-" is 8 chars) so an unrelated file is not
    // silently chosen -- a wrong .pfs is worse than none.
    return bestScore >= 8 ? best : QString();
}

/// Wait without freezing the window. The trigger board's settle window is
/// ~9.5 s; blocking the GUI thread for that long would make the app look hung
/// exactly when the operator is watching to see whether the run started.
void SleepResponsive(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

}  // namespace

MainWindow::MainWindow() {
    setWindowTitle("RatCam Recorder");
    resize(1100, 780);

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addWidget(BuildSettings());

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(BuildPreview());
    splitter->addWidget(BuildCameras());
    splitter->addWidget(BuildLog());
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 2);
    layout->addWidget(splitter, 1);

    layout->addWidget(BuildRunBar());
    setCentralWidget(central);

    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &MainWindow::OnTick);

    BuildMenus();

    // Remember the operator's choice between sessions.
    QSettings settings("RatCam", "RatCamRecorder");
    ApplyTheme(settings.value("darkMode", false).toBool());

    statusBar()->showMessage("Ready");
    OnDetect();
}

MainWindow::~MainWindow() = default;

QWidget* MainWindow::BuildSettings() {
    auto* box = new QGroupBox("Recording");
    auto* outer = new QVBoxLayout(box);

    auto* folderRow = new QHBoxLayout;
    folderEdit_ = new QLineEdit;
    folderEdit_->setPlaceholderText("F:/Temp_Video/session_1");
    auto* browse = new QPushButton("Browse...");
    connect(browse, &QPushButton::clicked, this, &MainWindow::OnPickFolder);
    folderRow->addWidget(new QLabel("Output folder"));
    folderRow->addWidget(folderEdit_, 1);
    folderRow->addWidget(browse);
    outer->addLayout(folderRow);

    auto* form = new QFormLayout;

    secondsSpin_ = new QSpinBox;
    secondsSpin_->setRange(1, 24 * 3600);
    secondsSpin_->setValue(60);
    secondsSpin_->setSuffix(" s");
    form->addRow("Duration", secondsSpin_);

    fpsSpin_ = new QDoubleSpinBox;
    // Below 2.5 the firmware treats the value as an LED command, not a rate,
    // so the pulse train would never start. Do not allow it to be selected.
    fpsSpin_->setRange(2.5, 1000.0);
    fpsSpin_->setDecimals(2);
    fpsSpin_->setValue(30.0);
    fpsSpin_->setSuffix(" fps");
    fpsSpin_->setToolTip("Rates below 2.5 are reserved by the trigger firmware "
                         "for LED commands and would not start the pulse train.");
    form->addRow("Frame rate", fpsSpin_);

    qpSpin_ = new QSpinBox;
    qpSpin_->setRange(0, 51);
    qpSpin_->setValue(21);
    form->addRow("Quality (qp, lower = better)", qpSpin_);

    codecCombo_ = new QComboBox;
    codecCombo_->addItems({"h264_nvenc", "hevc_nvenc", "libx264"});
    form->addRow("Codec", codecCombo_);

    auto* pfsRow = new QHBoxLayout;
    pfsEdit_ = new QLineEdit;
    auto* pfsPick = new QPushButton("...");
    pfsPick->setMaximumWidth(36);
    connect(pfsPick, &QPushButton::clicked, this, [this] {
        // Open where the rig's settings files are, so the right one is the
        // easy choice: picking a .pfs for another model fails every camera.
        QString start = pfsEdit_->text();
        if (start.isEmpty()) {
            for (const QString& guess : {QStringLiteral("E:/campy/campy/configs"),
                                         QDir::currentPath() + "/../configs"}) {
                if (QDir(guess).exists()) { start = guess; break; }
            }
        }
        const QString p = QFileDialog::getOpenFileName(
            this, "Camera settings", start, "pylon (*.pfs)");
        if (!p.isEmpty()) pfsEdit_->setText(p);
    });
    pfsRow->addWidget(pfsEdit_, 1);
    pfsRow->addWidget(pfsPick);
    form->addRow("Camera settings (.pfs)", pfsRow);

    auto* trigRow = new QHBoxLayout;
    portCombo_ = new QComboBox;
    portCombo_->setEditable(true);
    pinSpin_ = new QSpinBox;
    pinSpin_->setRange(0, 99);
    pinSpin_->setValue(39);
    pinSpin_->setPrefix("pin ");
    // Arm delay: how long the board holds the sync pins LOW after sending the
    // START packet, giving the IR device time to come up before the first
    // camera trigger. Sent to the board with DELAY (v3 firmware) rather than
    // waited out here -- the host never needs to know the value it set.
    armSpin_ = new QSpinBox;
    armSpin_->setRange(0, 60000);
    armSpin_->setValue(9000);
    armSpin_->setSuffix(" ms arm");
    armSpin_->setSingleStep(500);
    armSpin_->setToolTip("Sent to the trigger board as DELAY. It holds the sync "
                         "pins LOW for this long after the START packet, so the "
                         "IR device is running before the first camera trigger. "
                         "Ignored by v2 firmware, which uses its built-in 9000 ms.");
    trigRow->addWidget(portCombo_, 1);
    trigRow->addWidget(pinSpin_);
    trigRow->addWidget(armSpin_);
    form->addRow("Trigger", trigRow);

    auto* topRow = new QHBoxLayout;
    topViewCheck_ = new QCheckBox("Record the top-view camera (CAM0)");
    // Off by default: the rig runs six cameras, and the top view only halves
    // its trigger rate for reasons that are still unresolved. Ticking it is a
    // deliberate act, not the default state.
    topViewCheck_->setChecked(false);
    topViewCheck_->setToolTip(
        "CAM0 is optional. It shares the trigger but captures one frame per N "
        "pulses, so it records at a lower rate and will have proportionally "
        "fewer frames -- that is expected, not dropped frames. "
        "Unticked, CAM0 is left alone entirely and only the other cameras record.");
    topViewDivSpin_ = new QSpinBox;
    topViewDivSpin_->setRange(1, 10);
    topViewDivSpin_->setValue(2);
    topViewDivSpin_->setPrefix("1 frame per ");
    topViewDivSpin_->setSuffix(" triggers");
    connect(topViewCheck_, &QCheckBox::toggled,
            topViewDivSpin_, &QSpinBox::setEnabled);
    topRow->addWidget(topViewCheck_);
    topRow->addWidget(topViewDivSpin_);
    topRow->addStretch(1);
    form->addRow("Top view", topRow);

    buffersSpin_ = new QSpinBox;
    buffersSpin_->setRange(60, 6000);
    buffersSpin_->setValue(1500);
    buffersSpin_->setToolTip("Grab buffers per camera, and the whole tolerance "
                             "for the host briefly falling behind. 1500 is 50 s "
                             "of slack at 30 Hz.\n"
                             "The encoder ring is three quarters of this, so "
                             "raising this is what stops a disk or driver "
                             "hiccup costing frames.\n"
                             "Total RAM use is capped automatically.");
    form->addRow("Buffers per camera", buffersSpin_);

    outer->addLayout(form);
    return box;
}

void MainWindow::BuildMenus() {
    QMenu* view = menuBar()->addMenu("&View");
    auto* group = new QActionGroup(this);
    group->setExclusive(true);

    lightAction_ = view->addAction("&Light mode");
    darkAction_  = view->addAction("&Dark mode");
    for (QAction* a : {lightAction_, darkAction_}) {
        a->setCheckable(true);
        group->addAction(a);
        connect(a, &QAction::triggered, this, &MainWindow::OnThemeChanged);
    }
}

void MainWindow::OnThemeChanged() {
    ApplyTheme(darkAction_->isChecked());
}

void MainWindow::ApplyTheme(bool dark) {
    // Fusion honours a custom palette on every platform; the native Windows
    // style ignores most of it, so a dark palette there only half applies.
    qApp->setStyle(QStyleFactory::create("Fusion"));
    qApp->setPalette(dark ? DarkPalette() : qApp->style()->standardPalette());

    if (darkAction_)  darkAction_->setChecked(dark);
    if (lightAction_) lightAction_->setChecked(!dark);

    QSettings("RatCam", "RatCamRecorder").setValue("darkMode", dark);
}

QWidget* MainWindow::BuildPreview() {
    auto* box = new QGroupBox("Live view");
    auto* outer = new QVBoxLayout(box);
    outer->setContentsMargins(4, 4, 4, 4);

    auto* bar = new QHBoxLayout;
    detachButton_ = new QPushButton("Detach to its own window");
    detachButton_->setToolTip("Open the live view as a separate window, on the "
                              "second monitor if there is one. F11 there "
                              "toggles full screen.");
    connect(detachButton_, &QPushButton::clicked, this, &MainWindow::OnDetachPreview);
    bar->addStretch(1);
    bar->addWidget(detachButton_);
    outer->addLayout(bar);

    preview_ = new PreviewGrid;
    // Six cameras as 2 rows x 3 columns whenever the pane is wide enough.
    preview_->SetPreferredColumns(3);
    previewSlot_ = outer;
    outer->addWidget(preview_, 1);
    return box;
}

void MainWindow::OnDetachPreview() {
    if (previewWindow_) {          // already out: bring it forward instead
        previewWindow_->raise();
        previewWindow_->activateWindow();
        return;
    }

    // The grid is moved, not copied. A second grid would need its own feed from
    // the cameras, and two feeds drift apart.
    previewSlot_->removeWidget(preview_);
    previewWindow_ = new PreviewWindow(preview_, this);
    connect(previewWindow_, &PreviewWindow::Closing,
            this, &MainWindow::OnPreviewWindowClosed);
    previewWindow_->ShowOnSecondScreen();

    detachButton_->setText("Live view is in its own window");
    detachButton_->setEnabled(false);
    Say("Live view detached. Close that window to dock it again.");
}

void MainWindow::OnPreviewWindowClosed() {
    if (!previewWindow_) return;

    preview_->setParent(nullptr);
    previewSlot_->addWidget(preview_, 1);
    preview_->SetPreferredColumns(3);
    preview_->show();

    previewWindow_->deleteLater();
    previewWindow_ = nullptr;

    detachButton_->setText("Detach to its own window");
    detachButton_->setEnabled(true);
    Say("Live view docked.");
}

QWidget* MainWindow::BuildCameras() {
    auto* box = new QGroupBox("Cameras");
    auto* outer = new QVBoxLayout(box);

    auto* row = new QHBoxLayout;
    detectButton_ = new QPushButton("Detect cameras");
    connect(detectButton_, &QPushButton::clicked, this, &MainWindow::OnDetect);
    row->addWidget(detectButton_);
    row->addStretch(1);
    outer->addLayout(row);

    table_ = new QTableWidget(0, kHeaders.size());
    table_->setHorizontalHeaderLabels(kHeaders);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    outer->addWidget(table_, 1);
    return box;
}

QWidget* MainWindow::BuildLog() {
    auto* box = new QGroupBox("Log");
    auto* layout = new QVBoxLayout(box);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    QFont f = log_->font();
    f.setFamily("Consolas");
    log_->setFont(f);
    layout->addWidget(log_);
    return box;
}

QWidget* MainWindow::BuildRunBar() {
    auto* w = new QWidget;
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(true);

    verdictLabel_ = new QLabel;
    verdictLabel_->setMinimumWidth(180);

    stopButton_ = new QPushButton("Stop");
    stopButton_->setEnabled(false);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::OnStop);

    startButton_ = new QPushButton("START");
    startButton_->setMinimumHeight(38);
    startButton_->setStyleSheet("font-weight: bold;");
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::OnStart);

    row->addWidget(progress_, 1);
    row->addWidget(verdictLabel_);
    row->addWidget(stopButton_);
    row->addWidget(startButton_);
    return w;
}

void MainWindow::Say(const QString& text, const QString& level) {
    const QString prefix = level == "error" ? "X " : level == "warn" ? "! " : "  ";
    log_->appendPlainText(prefix + text);
}

void MainWindow::OnPickFolder() {
    const QString p = QFileDialog::getExistingDirectory(this, "Output folder");
    if (!p.isEmpty()) folderEdit_->setText(p);
}

void MainWindow::OnDetect() {
    // Score every serial port and pre-select the most Arduino-looking one, so
    // the operator does not have to know which COM number the board landed on.
    const QString previous = portCombo_->currentData().toString();
    portCombo_->clear();

    int bestRow = -1, bestScore = 0;
    const auto ports = QSerialPortInfo::availablePorts();
    for (int i = 0; i < ports.size(); ++i) {
        QString why;
        const int score = ScorePort(ports[i], why);
        QString label = ports[i].portName() + "  --  " + why;
        if (ports[i].hasVendorIdentifier() && ports[i].hasProductIdentifier())
            label += QString("  [%1:%2]")
                .arg(ports[i].vendorIdentifier(), 4, 16, QChar('0'))
                .arg(ports[i].productIdentifier(), 4, 16, QChar('0')).toUpper();
        portCombo_->addItem(label, ports[i].portName());
        if (score > bestScore) { bestScore = score; bestRow = i; }
    }

    // A port the operator already chose wins over the guess.
    const int previousRow = portCombo_->findData(previous);
    if (!previous.isEmpty() && previousRow >= 0) {
        portCombo_->setCurrentIndex(previousRow);
    } else if (bestRow >= 0) {
        portCombo_->setCurrentIndex(bestRow);
        Say(QString("Trigger board: %1").arg(portCombo_->itemText(bestRow)));
    } else if (!ports.isEmpty()) {
        Say("No port looks like an Arduino; choose one manually.", "warn");
    }

    table_->setRowCount(0);
    try {
        Pylon::DeviceInfoList_t devices;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(devices);
        table_->setRowCount(static_cast<int>(devices.size()));
        for (size_t i = 0; i < devices.size(); ++i) {
            QString name = QString::fromUtf8(devices[i].GetUserDefinedName().c_str());
            if (name.isEmpty()) name = QString("CAM%1").arg(i + 1);
            table_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(name));
            table_->setItem(static_cast<int>(i), 1, new QTableWidgetItem(
                QString::fromUtf8(devices[i].GetSerialNumber().c_str())));
        }
        preview_->SetCameraCount(static_cast<int>(devices.size()));
        for (size_t i = 0; i < devices.size(); ++i) {
            QString nm = QString::fromUtf8(devices[i].GetUserDefinedName().c_str());
            if (nm.isEmpty()) nm = QString("CAM%1").arg(i + 1);
            preview_->SetLabel(static_cast<int>(i), nm, false);
        }
        Say(QString("Detected %1 camera(s), %2 serial port(s)")
                .arg(devices.size()).arg(portCombo_->count()));

        if (!devices.empty() && pfsEdit_->text().isEmpty()) {
            const QString model =
                QString::fromUtf8(devices[0].GetModelName().c_str());
            const QString pfs = BestPfsForModel(model);
            if (!pfs.isEmpty()) {
                pfsEdit_->setText(pfs);
                Say(QString("Settings file for %1: %2").arg(model, pfs));
            } else {
                Say(QString("No .pfs matching %1 found; choose one manually.")
                        .arg(model), "warn");
            }
        }
    } catch (const Pylon::GenericException& e) {
        Say(QString("Camera enumeration failed: %1").arg(e.GetDescription()), "error");
    }
}

bool MainWindow::ConfirmPreflight() {
    // Build the camera list fresh rather than trusting the table, so what the
    // operator confirms is what the recorder is about to open.
    std::vector<PreflightDialog::CameraLine> cams;
    try {
        Pylon::DeviceInfoList_t devices;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(devices);
        for (size_t i = 0; i < devices.size(); ++i) {
            PreflightDialog::CameraLine c;
            c.name = QString::fromUtf8(devices[i].GetUserDefinedName().c_str());
            if (c.name.isEmpty()) c.name = QString("CAM%1").arg(i + 1);
            c.serial = QString::fromUtf8(devices[i].GetSerialNumber().c_str());
            c.ok = true;
            cams.push_back(c);
        }
    } catch (const Pylon::GenericException& e) {
        PreflightDialog::CameraLine c;
        c.name = "enumeration failed";
        c.note = QString::fromUtf8(e.GetDescription());
        c.ok = false;
        cams.push_back(c);
    }

    PreflightDialog dlg(folderEdit_->text().trimmed(), secondsSpin_->value(),
                        fpsSpin_->value(), cams,
                        static_cast<int>(cams.size()), this);
    return dlg.exec() == QDialog::Accepted;
}

void MainWindow::SetRunning(bool running) {
    running_ = running;
    startButton_->setEnabled(!running);
    stopButton_->setEnabled(running);
    detectButton_->setEnabled(!running);
    folderEdit_->setEnabled(!running);
    secondsSpin_->setEnabled(!running);
    fpsSpin_->setEnabled(!running);
    qpSpin_->setEnabled(!running);
    codecCombo_->setEnabled(!running);
    pfsEdit_->setEnabled(!running);
    portCombo_->setEnabled(!running);
    pinSpin_->setEnabled(!running);
    armSpin_->setEnabled(!running);
    buffersSpin_->setEnabled(!running);
    topViewCheck_->setEnabled(!running);
    topViewDivSpin_->setEnabled(!running && topViewCheck_->isChecked());
}

void MainWindow::OnStart() {
    if (running_) return;

    const QString folder = folderEdit_->text().trimmed();
    if (folder.isEmpty()) {
        QMessageBox::warning(this, "RatCam Recorder", "Choose an output folder first.");
        return;
    }
    // Never silently destroy a previous recording.
    const std::filesystem::path f = folder.toStdString();
    if (std::filesystem::exists(f) && !std::filesystem::is_empty(f)) {
        QMessageBox::warning(this, "RatCam Recorder",
            QString("%1 already exists and is not empty.\n\n"
                    "Recording would overwrite it. Choose another folder.")
                .arg(folder));
        return;
    }

    // Last chance to catch a stale folder, a wrong duration or a missing
    // camera -- all three have gone wrong on this rig, and a session is not
    // repeatable once the animal has been handled.
    if (!ConfirmPreflight()) {
        Say("Cancelled at the pre-flight check; nothing was recorded.");
        return;
    }

    Recorder::Settings rs;
    rs.videoFolder  = folder.toStdString();
    rs.frameRate    = fpsSpin_->value();
    rs.qp           = qpSpin_->value();
    rs.codec        = codecCombo_->currentText().toStdString();
    rs.maxNumBuffer = buffersSpin_->value();

    try {
        Pylon::DeviceInfoList_t devices;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(devices);
        if (devices.empty()) {
            QMessageBox::warning(this, "RatCam Recorder", "No cameras detected.");
            return;
        }
        int skipped = 0;
        for (size_t i = 0; i < devices.size(); ++i) {
            const QString model  = QString::fromUtf8(devices[i].GetModelName().c_str());
            const QString serial = QString::fromUtf8(devices[i].GetSerialNumber().c_str());
            QString name = QString::fromUtf8(devices[i].GetUserDefinedName().c_str());
            if (name.isEmpty()) name = QString("CAM%1").arg(i + 1);

            Recorder::CameraSpec spec;
            spec.serial  = serial.toStdString();
            spec.name    = name.toStdString();
            spec.pfsPath = pfsEdit_->text().toStdString();

            if (IsTopView(name, model, serial)) {
                if (!topViewCheck_->isChecked()) { ++skipped; continue; }
                // Its own settings file: a different model, resolution and
                // pixel format, so the main .pfs would fail on it outright.
                const QString topPfs = BestPfsForModel(model);
                if (topPfs.isEmpty()) {
                    Say(QString("%1: no .pfs found for %2; skipping the top view.")
                            .arg(name, model), "warn");
                    ++skipped;
                    continue;
                }
                spec.pfsPath = topPfs.toStdString();
                spec.triggerDivider = topViewDivSpin_->value();
                Say(QString("%1 (top view, %2): %3, 1 frame per %4 triggers -> %5 fps")
                        .arg(name, model, topPfs)
                        .arg(spec.triggerDivider)
                        .arg(fpsSpin_->value() / spec.triggerDivider, 0, 'g', 4));
            }
            rs.cameras.push_back(spec);
        }
        if (skipped)
            Say(QString("Top view not recorded (%1 camera skipped).").arg(skipped));
    } catch (const Pylon::GenericException& e) {
        Say(QString("Enumeration failed: %1").arg(e.GetDescription()), "error");
        return;
    }

    log_->clear();
    Say(QString("Recording %1 camera(s) for %2 s into %3")
            .arg(rs.cameras.size()).arg(secondsSpin_->value()).arg(folder));

    recorder_ = std::make_unique<Recorder>(rs);

    const QString port = portCombo_->currentData().isValid()
        ? portCombo_->currentData().toString()
        : portCombo_->currentText().section(' ', 0, 0);
    const int pin = pinSpin_->value();
    const double rate = fpsSpin_->value();
    const int armMs = armSpin_->value();

    startupError_.clear();
    startup_ = Startup::Working;
    SetRunning(true);
    stopButton_->setEnabled(false);          // nothing to stop until armed
    progress_->setFormat("starting...");
    Say("Opening cameras...");

    // All of this blocks: six camera opens, then a trigger handshake whose
    // retry path can wait tens of seconds. Off the GUI thread so the window
    // stays alive and the log keeps updating.
    startThread_ = std::thread([this, port, pin, rate, armMs] {
        if (!recorder_->Open()) { startup_ = Startup::Failed; return; }

        // Arm every camera BEFORE the trigger, so pulse #1 reaches all of them
        // at once and frame 0 is the same instant in every file.
        recorder_->Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        if (!port.isEmpty()) {
            trigger_ = std::make_unique<SerialTrigger>();
            if (trigger_->Open(port.toStdString())) {
                // v3 accepts the arm delay; v2 does not and keeps its built-in
                // 9000 ms. Either is fine -- the host counts frames, not time.
                std::string version;
                if (trigger_->QueryVersion(version)) trigger_->SetDelay(armMs);
            }
            if (!trigger_->IsOpen() ||
                !trigger_->Start(pin, rate)) {
                startupError_ = trigger_->LastError();
                recorder_->Stop();
                trigger_.reset();
                startup_ = Startup::Failed;
                return;
            }
            // No wait here. The board sends its START packet and then waits
            // its own START_SETTLE_MS before the first pulse; the cameras are
            // already armed and simply capture whatever arrives. Recording
            // length is counted in frames, so the firmware's delay -- 9000 ms
            // today, 500 ms after a reflash -- needs no counterpart in the host.
        }
        startup_ = Startup::Ready;
    });

    timer_->start();
    return;
}

void MainWindow::OnStartupFinished() {
    if (startThread_.joinable()) startThread_.join();

    if (startup_.load() == Startup::Failed) {
        Say("Setup failed; nothing was recorded.", "error");
        if (!startupError_.empty())
            Say(QString::fromStdString(startupError_), "error");
        if (recorder_) {
            for (const auto& st : recorder_->Status())
                if (st.failed && !st.error.empty())
                    Say(QString("%1: %2").arg(QString::fromStdString(st.name),
                                              QString::fromStdString(st.error)),
                        "error");
        }
        if (!pfsEdit_->text().isEmpty())
            Say("If the errors say \"node not found\", the .pfs is for a "
                "different camera model.", "warn");
        QMessageBox::critical(this, "RatCam Recorder",
            "Cameras could not be set up, so nothing was recorded.\n\n"
            "See the log for detail.");
        timer_->stop();
        recorder_.reset();
        startup_ = Startup::Idle;
        SetRunning(false);
        progress_->setFormat("");
        return;
    }

    Say("Recording.");
    const size_t nCams = recorder_->Status().size();
    stallReported_.assign(nCams, false);
    prevFrames_.assign(nCams, 0);
    previewSerials_.assign(nCams, 0);
    preview_->SetCameraCount(static_cast<int>(nCams));
    plannedSeconds_ = secondsSpin_->value();
    startMs_ = QDateTime::currentMSecsSinceEpoch();
    lastProgressFrames_ = 0;
    lastProgressMs_ = startMs_;
    reportedFailure_ = false;
    verdictLabel_->clear();
    stopButton_->setEnabled(true);
}

void MainWindow::OnTick() {
    if (!recorder_) return;

    const Startup phase = startup_.load();
    if (phase == Startup::Working) return;               // still opening/arming
    if (phase == Startup::Ready || phase == Startup::Failed) {
        startup_ = Startup::Idle;
        OnStartupFinished();
        return;
    }

    const auto status = recorder_->Status();
    const double elapsed = (QDateTime::currentMSecsSinceEpoch() - startMs_) / 1000.0;

    // Frames, not seconds: the board waits its own settle before the first
    // pulse, so a wall clock would count that silence against the recording
    // and come up short.
    const uint64_t target = static_cast<uint64_t>(
        plannedSeconds_ * fpsSpin_->value() + 0.5);
    const uint64_t got = recorder_->MinFramesReceived();
    progress_->setValue(target ? qBound(0, int(got * 100 / target), 100) : 0);
    progress_->setFormat(got == 0
        ? QString("waiting for trigger... (%1 s)").arg(elapsed, 0, 'f', 0)
        : QString("%1 / %2 frames").arg(got).arg(target));

    if (table_->rowCount() < static_cast<int>(status.size()))
        table_->setRowCount(static_cast<int>(status.size()));

    if (stallReported_.size() < status.size()) stallReported_.resize(status.size(), false);

    for (size_t i = 0; i < status.size(); ++i) {
        const auto& s = status[i];
        const int r = static_cast<int>(i);
        const uint64_t received = s.health.framesReceived;
        const double fps = prevFrames_[i] ? (received - prevFrames_[i]) * 2.0 : 0.0;
        prevFrames_[i] = received;

        const size_t pct = s.ring.capacity ? s.ring.size * 100 / s.ring.capacity : 0;
        const QStringList cells = {
            QString::fromStdString(s.name),
            QString::fromStdString(s.serial),
            QString::number(fps, 'f', 0),
            QString::number(received),
            QString::number(s.framesEncoded),
            QString::number(s.health.framesLost),
            QString::number(s.health.framesDropped),
            QString("%1%").arg(pct),
        };
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = table_->item(r, c);
            if (!item) { item = new QTableWidgetItem; table_->setItem(r, c, item); }
            item->setText(cells[c]);
        }
        // Loss and drops are the numbers that matter; make them impossible to
        // miss while the animal is still in the arena, not after the session.
        // An encoder that has not returned from a call for over a second is
        // already a hundred frames behind. Catching it here is the difference
        // between losing one minute and losing the rest of the session: in the
        // first long recording it went unnoticed for 26 s and cost 793 frames
        // on every camera.
        const bool stalled = s.encoderStallMs >= 1000;
        if (stalled && !stallReported_[i]) {
            stallReported_[i] = true;
            Say(QString("%1: ENCODER STALLED %2 s in %3 -- frames are queuing "
                        "and will be lost if it does not recover")
                    .arg(QString::fromStdString(s.name))
                    .arg(s.encoderStallMs / 1000.0, 0, 'f', 1)
                    .arg(QString::fromStdString(s.encoderPhase)), "error");
        } else if (!stalled && stallReported_[i]) {
            stallReported_[i] = false;
            Say(QString("%1: encoder recovered (worst stall %2 s in %3)")
                    .arg(QString::fromStdString(s.name))
                    .arg(s.worstStallMs / 1000.0, 0, 'f', 1)
                    .arg(QString::fromStdString(s.worstStallPhase)), "warn");
        }

        const bool bad = s.health.framesLost || s.health.framesDropped ||
                         s.failed || stalled;

        // Live view. TakePreview returns false when nothing new has arrived,
        // so a stalled camera simply keeps its last image rather than blanking.
        std::vector<uint8_t> rgb;
        int pw = 0, ph = 0;
        if (i < previewSerials_.size() &&
            recorder_->TakePreview(i, rgb, pw, ph, previewSerials_[i]))
            preview_->SetImage(r, rgb, pw, ph);

        QString caption = QString("%1  %2 fps")
            .arg(QString::fromStdString(s.name)).arg(fps, 0, 'f', 0);
        if (s.health.framesLost)    caption += QString("  LOST %1").arg(s.health.framesLost);
        if (s.health.framesDropped) caption += QString("  DROPPED %1").arg(s.health.framesDropped);
        if (stalled) caption += QString("  STALLED %1s").arg(s.encoderStallMs / 1000.0, 0, 'f', 0);
        preview_->SetLabel(r, caption, bad);
        for (int c = 0; c < cells.size(); ++c)
            table_->item(r, c)->setBackground(bad ? QColor(255, 210, 210)
                                                  : QBrush());
    }

    // Say it once, loudly, the moment a camera drops out -- not at the end.
    if (!reportedFailure_ && recorder_->AnyCameraFailed()) {
        reportedFailure_ = true;
        for (const auto& s2 : status) {
            if (!s2.failed) continue;
            Say(QString("%1 DROPPED OUT: %2").arg(
                    QString::fromStdString(s2.name),
                    QString::fromStdString(s2.error)), "error");
        }
        Say("The remaining cameras keep recording and will finish normally. "
            "A camera that is plugged back in does NOT rejoin this session.",
            "warn");
    }

    if (got >= target) { FinishRun("frame count reached"); return; }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (got > lastProgressFrames_) {
        lastProgressFrames_ = got;
        lastProgressMs_ = nowMs;
    }

    // Safety net: if no frame ever arrives the trigger is not running, and
    // waiting forever would look like a hang.
    if (got == 0 && elapsed > 60.0) {
        Say("No frames after 60 s -- the trigger does not appear to be pulsing.",
            "error");
        FinishRun("no trigger");
        return;
    }

    // Stall watchdog. Once frames are flowing they arrive many times a second,
    // so 15 s without a single one means something has genuinely stopped --
    // every camera lost, or the trigger cut. It cannot fire during a healthy
    // recording, where this counter advances ~30 times a second.
    if (got > 0 && (nowMs - lastProgressMs_) > 15000) {
        Say(QString("No new frames for 15 s (stopped at %1 of %2). Ending the "
                    "session so the files are finalized.").arg(got).arg(target),
            "error");
        FinishRun("stalled");
    }
}

void MainWindow::OnStop() {
    if (!running_) return;
    FinishRun("stopped by user");
}

void MainWindow::FinishRun(const QString& reason) {
    timer_->stop();
    if (startThread_.joinable()) startThread_.join();
    stopButton_->setEnabled(false);
    Say(QString("Stopping (%1)...").arg(reason));
    QApplication::processEvents();

    // 1. Stop the trigger FIRST. Finalizing 50 GB of video takes minutes, and
    //    the Arduino used to keep pulsing throughout it -- the cameras were
    //    still being triggered long after the session had ended, with nothing
    //    left to receive the frames. Stopping the trigger is the one part of
    //    shutdown that must not wait behind the slow part.
    if (trigger_) {
        Say(QString("Trigger: stopping pin %1").arg(pinSpin_->value()));
        QApplication::processEvents();
        if (!trigger_->Stop(pinSpin_->value()))
            Say(QString::fromStdString(trigger_->LastError()), "warn");
        trigger_->Close();
        trigger_.reset();
    }

    // 2. Let the frames already in flight arrive. Cutting the cameras off the
    //    instant the trigger stops would discard frames that were genuinely
    //    captured, and would end the cameras at different BlockIDs.
    if (recorder_) {
        Say("Waiting for the last frames to arrive...");
        QApplication::processEvents();
        const uint64_t before = recorder_->TotalFramesReceived();
        const bool quiet = recorder_->WaitUntilQuiet(
            1000, 15000, [this](uint64_t, int) { QApplication::processEvents(); });
        const uint64_t after = recorder_->TotalFramesReceived();
        if (!quiet)
            Say("Frames were still arriving after 15 s -- is the trigger really "
                "stopped? Finalizing anyway.", "warn");
        else
            Say(QString("Cameras quiet (%1 frame(s) arrived after the trigger stopped).")
                    .arg(after - before));
        QApplication::processEvents();
    }

    // 3. Only now shut the encoders down and finalize the files.
    if (recorder_) {
        Say("Draining buffers and finalizing video files...");
        QApplication::processEvents();
        recorder_->Stop();

        Say("Writing metadata (frametimes, gaps, session report)...");
        QApplication::processEvents();
        const auto reports = recorder_->WriteMetadata();
        for (const auto& r : reports) {
            if (r.framesMissing == 0) continue;
            // Say exactly which frames went missing, not just that some did.
            Say(QString("%1: %2 frame(s) MISSING in %3 gap(s), %4 s lost")
                    .arg(QString::fromStdString(r.name))
                    .arg(r.framesMissing).arg(r.gaps.size())
                    .arg(r.lostSeconds, 0, 'f', 4), "error");
            for (const auto& g : r.gaps)
                Say(QString("    after frame %1: BlockID %2..%3 missing "
                            "(%4 frame(s)), t=%5..%6 s, gap %7 s")
                        .arg(g.afterFrame).arg(g.firstMissingBlock)
                        .arg(g.lastMissingBlock).arg(g.missingCount)
                        .arg(g.timeBefore, 0, 'f', 6).arg(g.timeAfter, 0, 'f', 6)
                        .arg(g.gapSeconds, 0, 'f', 6), "error");
        }
    }

    // The trigger was already stopped at the top of this function, before the
    // slow finalize -- nothing to do here.

    uint64_t lost = 0, dropped = 0;
    bool aligned = true;
    uint64_t firstBlock = 0;
    if (recorder_) {
        const auto status = recorder_->Status();
        for (size_t i = 0; i < status.size(); ++i) {
            lost += status[i].health.framesLost;
            dropped += status[i].health.framesDropped;
            if (i == 0) firstBlock = status[i].health.firstBlockId;
            else if (status[i].health.firstBlockId != firstBlock) aligned = false;
            Say(QString("%1  received %2  encoded %3  lost %4  dropped %5  "
                        "blocks %6..%7")
                    .arg(QString::fromStdString(status[i].name))
                    .arg(status[i].health.framesReceived)
                    .arg(status[i].framesEncoded)
                    .arg(status[i].health.framesLost)
                    .arg(status[i].health.framesDropped)
                    .arg(status[i].health.firstBlockId)
                    .arg(status[i].health.lastBlockId));
        }
        Say(aligned
            ? QString("Alignment: every camera starts at BlockID %1 -- frame 0 is "
                      "the same trigger pulse everywhere.").arg(firstBlock)
            : QString("Alignment: cameras start at different BlockIDs; the videos "
                      "are offset."), aligned ? QString() : "warn");
    }

    const bool ok = lost == 0 && dropped == 0 && aligned;
    verdictLabel_->setText(ok ? "PASS" : "CHECK LOG");
    verdictLabel_->setStyleSheet(ok ? "color: #0a7a0a; font-weight: bold;"
                                    : "color: #b00020; font-weight: bold;");
    Say(ok ? "PASS -- nothing lost, nothing dropped, every file finalized."
           : QString("ATTENTION -- %1 lost on the wire, %2 dropped at the encoder.")
                 .arg(lost).arg(dropped),
        ok ? QString() : "error");

    recorder_.reset();
    SetRunning(false);
    progress_->setValue(0);
    progress_->setFormat("");
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (previewWindow_) {
        // Take the grid back first so it is destroyed with this window, not
        // left orphaned by the closing child.
        preview_->setParent(this);
        previewWindow_->close();
    }

    if (running_) {
        const auto answer = QMessageBox::question(this, "RatCam Recorder",
            "A recording is running. Stop it cleanly and quit?\n\n"
            "Buffers will be drained and every file finalized first.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) { event->ignore(); return; }
        FinishRun("window closed");
    }
    event->accept();
}

} // namespace campy
