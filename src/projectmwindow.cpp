#include "projectmwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSettings>
#include <QTimer>
#include <QFileInfo>

ProjectMWindow::ProjectMWindow(QWidget* parent)
    : QWidget(parent, Qt::Window)
    , m_audioEngine(nullptr)
{
    setWindowTitle("projectM Visualizer - Deezer Client");
    resize(1024, 768);

    // Don't prevent application from quitting when this window is still open
    setAttribute(Qt::WA_QuitOnClose, false);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Control bar
    QWidget* controlBar = new QWidget(this);
    controlBar->setStyleSheet(R"(
        QWidget {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 #2a2a2a, stop:1 #1a1a1a);
            color: white;
        }
    )");
    controlBar->setFixedHeight(50);

    QHBoxLayout* controlLayout = new QHBoxLayout(controlBar);

    // Preset navigation buttons
    m_prevButton = new QPushButton("◄ Previous", controlBar);
    m_nextButton = new QPushButton("Next ►", controlBar);
    m_randomButton = new QPushButton("Random", controlBar);

    QString btnStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 #4a4a4a, stop:1 #2a2a2a);
            color: white;
            border: 1px solid #555;
            border-radius: 3px;
            padding: 6px 15px;
            font-weight: bold;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 #5a5a5a, stop:1 #3a3a3a);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 #2a2a2a, stop:1 #4a4a4a);
        }
    )";

    m_prevButton->setStyleSheet(btnStyle);
    m_nextButton->setStyleSheet(btnStyle);
    m_randomButton->setStyleSheet(btnStyle);

    // Preset name label
    m_presetLabel = new QLabel("Loading projectM...", controlBar);
    m_presetLabel->setStyleSheet(R"(
        QLabel {
            color: #00ff00;
            font-weight: bold;
            font-size: 14px;
            background: transparent;
        }
    )");
    m_presetLabel->setAlignment(Qt::AlignCenter);

    // Search box
    m_searchEdit = new QLineEdit(controlBar);
    m_searchEdit->setPlaceholderText("Search presets...");
    m_searchEdit->setMaximumWidth(200);
    m_searchEdit->setStyleSheet(R"(
        QLineEdit {
            background: #1a1a1a;
            color: white;
            border: 1px solid #555;
            border-radius: 3px;
            padding: 4px 8px;
        }
    )");

    // Search results dropdown
    m_searchResults = new QComboBox(controlBar);
    m_searchResults->setMaximumWidth(300);
    m_searchResults->setStyleSheet(R"(
        QComboBox {
            background: #1a1a1a;
            color: white;
            border: 1px solid #555;
            border-radius: 3px;
            padding: 4px 8px;
        }
        QComboBox::drop-down {
            border: none;
        }
        QComboBox::down-arrow {
            image: none;
            border-left: 4px solid transparent;
            border-right: 4px solid transparent;
            border-top: 6px solid white;
            width: 0;
            height: 0;
        }
    )");
    m_searchResults->hide();  // Hidden until search has results

    controlLayout->addWidget(m_prevButton);
    controlLayout->addWidget(m_randomButton);
    controlLayout->addWidget(m_nextButton);
    controlLayout->addStretch();
    controlLayout->addWidget(m_presetLabel);
    controlLayout->addStretch();
    controlLayout->addWidget(m_searchEdit);
    controlLayout->addWidget(m_searchResults);

    // ProjectM widget (OpenGL)
    m_projectMWidget = new ProjectMWidget(this);

    mainLayout->addWidget(controlBar);
    mainLayout->addWidget(m_projectMWidget, 1);

    // Forward debug log from widget to window
    connect(m_projectMWidget, &ProjectMWidget::debugLog,
            this, &ProjectMWindow::debugLog);

    // Connect preset navigation
    connect(m_prevButton, &QPushButton::clicked,
            m_projectMWidget, &ProjectMWidget::previousPreset);

    connect(m_nextButton, &QPushButton::clicked,
            m_projectMWidget, &ProjectMWidget::nextPreset);

    connect(m_randomButton, &QPushButton::clicked,
            m_projectMWidget, &ProjectMWidget::randomPreset);

    // Connect search functionality
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &ProjectMWindow::onSearchTextChanged);

    connect(m_searchResults, QOverload<int>::of(&QComboBox::activated),
            this, &ProjectMWindow::onPresetSelected);

    // Update preset label periodically
    QTimer* updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, &ProjectMWindow::updatePresetLabel);
    updateTimer->start(1000);  // Update every second

    // Initial update
    QTimer::singleShot(2000, this, &ProjectMWindow::updatePresetLabel);

    // Restore window geometry
    QSettings settings;
    QByteArray geometry = settings.value("ProjectMWindow/geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
}

ProjectMWindow::~ProjectMWindow()
{
}

void ProjectMWindow::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;

    if (m_audioEngine) {
        connect(m_audioEngine, &AudioEngine::spectrumDataReady,
                m_projectMWidget, &ProjectMWidget::setSpectrumData);
        connect(m_audioEngine, &AudioEngine::pcmDataReady,
                m_projectMWidget, &ProjectMWidget::setPCMData);

        // Start/pause visualizer rendering based on audio playback state
        connect(m_audioEngine, &AudioEngine::stateChanged, this, [this](AudioEngine::PlaybackState state) {
            if (state == AudioEngine::Playing) {
                m_projectMWidget->resumeRendering();
            } else {
                m_projectMWidget->pauseRendering();
            }
        });
    }
}

void ProjectMWindow::updatePresetLabel()
{
    if (m_projectMWidget->isInitialized()) {
        QString presetName = m_projectMWidget->currentPresetName();
        m_presetLabel->setText(presetName);
    } else {
        m_presetLabel->setText("projectM not initialized");
    }
}

void ProjectMWindow::onSearchTextChanged(const QString& text)
{
    if (text.trimmed().isEmpty()) {
        m_searchResults->hide();
        m_searchResults->clear();
        return;
    }

    // Get all preset files from ProjectMWidget
    QStringList allPresets = m_projectMWidget->presetFiles();

    // Filter presets by search text (case-insensitive)
    QStringList filtered;
    QString lowerSearch = text.toLower();

    for (const QString& preset : allPresets) {
        // Extract just the filename (without path) for matching
        QString filename = QFileInfo(preset).fileName();
        if (filename.toLower().contains(lowerSearch)) {
            filtered.append(preset);
        }
    }

    // Update combo box with results
    m_searchResults->clear();

    if (filtered.isEmpty()) {
        m_searchResults->addItem("No presets found");
        m_searchResults->setEnabled(false);
        m_searchResults->show();
    } else {
        for (const QString& preset : filtered) {
            // Show just the filename in the dropdown
            QString displayName = QFileInfo(preset).fileName();
            m_searchResults->addItem(displayName, preset);  // Store full path in user data
        }
        m_searchResults->setEnabled(true);
        m_searchResults->show();
    }
}

void ProjectMWindow::onPresetSelected(int index)
{
    if (index < 0 || !m_searchResults->isEnabled()) {
        return;
    }

    // Get the full preset path from user data
    QString presetPath = m_searchResults->itemData(index).toString();

    if (!presetPath.isEmpty()) {
        m_projectMWidget->loadPreset(presetPath);

        // Clear search
        m_searchEdit->clear();
        m_searchResults->hide();
    }
}

void ProjectMWindow::closeEvent(QCloseEvent* event)
{
    // Save geometry
    QSettings settings;
    settings.setValue("ProjectMWindow/geometry", saveGeometry());

    emit closed();
    QWidget::closeEvent(event);
}
