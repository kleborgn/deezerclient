#ifndef PROJECTMWINDOW_H
#define PROJECTMWINDOW_H

#include <QWidget>
#include <QCloseEvent>
#include "projectmwidget.h"
#include "audioengine.h"

class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;

class ProjectMWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectMWindow(QWidget* parent = nullptr);
    ~ProjectMWindow();

    void setAudioEngine(AudioEngine* engine);

protected:
    void closeEvent(QCloseEvent* event) override;

signals:
    void closed();
    void debugLog(const QString& message);

private slots:
    void updatePresetLabel();
    void onSearchTextChanged(const QString& text);
    void onPresetSelected(int index);

private:
    ProjectMWidget* m_projectMWidget;
    AudioEngine* m_audioEngine;

    QLabel* m_presetLabel;
    QPushButton* m_prevButton;
    QPushButton* m_nextButton;
    QPushButton* m_randomButton;
    QLineEdit* m_searchEdit;
    QComboBox* m_searchResults;
};

#endif // PROJECTMWINDOW_H
