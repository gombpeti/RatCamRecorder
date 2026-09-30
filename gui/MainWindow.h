#pragma once

// Qt front end for the Recorder.
//
// Deliberately thin: all acquisition logic lives in Recorder/CameraWorker/
// VideoEncoder, which the headless `record` tool drives the same way. The GUI
// adds configuration, a live health view and a Stop button -- it does not get
// its own copy of the pipeline.
//
// Stop here cannot corrupt anything. It signals the cameras, the rings drain
// into the encoders, and each encoder writes its trailer. There is no kill
// path, because there is no separate process to kill -- which is the whole
// reason the Python tool lost recordings.

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <thread>

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QTableWidget;
class QAction;
class QTimer;
class QVBoxLayout;

namespace campy { class PreviewGrid; class PreviewWindow; }

namespace campy {

class Recorder;
class SerialTrigger;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void OnDetect();
    void OnPickFolder();
    void OnStart();
    void OnStop();
    void OnTick();
    void OnStartupFinished();
    void OnThemeChanged();
    void OnDetachPreview();
    void OnPreviewWindowClosed();

private:
    QWidget* BuildSettings();
    QWidget* BuildCameras();
    QWidget* BuildPreview();
    void BuildMenus();
    void ApplyTheme(bool dark);
    bool ConfirmPreflight();
    QWidget* BuildLog();
    QWidget* BuildRunBar();

    void Say(const QString& text, const QString& level = QString());
    void SetRunning(bool running);
    void FinishRun(const QString& reason);

    // settings
    QLineEdit* folderEdit_ = nullptr;
    QSpinBox* secondsSpin_ = nullptr;
    QDoubleSpinBox* fpsSpin_ = nullptr;
    QSpinBox* qpSpin_ = nullptr;
    QComboBox* codecCombo_ = nullptr;
    QLineEdit* pfsEdit_ = nullptr;
    QComboBox* portCombo_ = nullptr;
    QSpinBox* pinSpin_ = nullptr;
    QSpinBox* armSpin_ = nullptr;
    QSpinBox* buffersSpin_ = nullptr;
    QCheckBox* topViewCheck_ = nullptr;
    QSpinBox* topViewDivSpin_ = nullptr;

    PreviewGrid* preview_ = nullptr;
    PreviewWindow* previewWindow_ = nullptr;   // non-null while detached
    QVBoxLayout* previewSlot_ = nullptr;       // where it lives when docked
    QPushButton* detachButton_ = nullptr;
    std::vector<uint64_t> previewSerials_;
    QTableWidget* table_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* detectButton_ = nullptr;
    QAction* darkAction_ = nullptr;
    QAction* lightAction_ = nullptr;
    QLabel* verdictLabel_ = nullptr;

    QTimer* timer_ = nullptr;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<SerialTrigger> trigger_;
    std::vector<uint64_t> prevFrames_;
    /// One entry per camera: whether its encoder is currently in a
    /// reported stall, so the log gets one line per episode rather than
    /// one per refresh.
    std::vector<bool> stallReported_;
    qint64 startMs_ = 0;
    // Stall watchdog: a camera that dies mid-recording stops counting, and
    // without this the session would wait for someone to press Stop.
    uint64_t lastProgressFrames_ = 0;
    qint64 lastProgressMs_ = 0;
    bool reportedFailure_ = false;
    int plannedSeconds_ = 0;
    bool running_ = false;

    // Opening six cameras and completing the trigger handshake takes seconds,
    // and SerialTrigger blocks on sleeps and serial reads. Doing that on the
    // GUI thread makes Windows paint the window "Not Responding" for the whole
    // startup, so it runs on its own thread and the timer watches this state.
    enum class Startup { Idle, Working, Ready, Failed };
    std::thread startThread_;
    std::atomic<Startup> startup_{Startup::Idle};
    std::string startupError_;
};

} // namespace campy
