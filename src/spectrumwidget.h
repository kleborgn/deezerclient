#ifndef SPECTRUMWIDGET_H
#define SPECTRUMWIDGET_H

#include <QWidget>
#include <QVector>
#include <QPainter>
#include <QColor>

class SpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrumWidget(QWidget *parent = nullptr);

    void setSpectrumData(const QVector<float>& magnitudes);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor getColorForMagnitude(float magnitude);

    QVector<float> m_magnitudes;
    static constexpr int NUM_BANDS = 32;
    static constexpr int BAR_WIDTH = 12;
    static constexpr int BAR_GAP = 2;
};

#endif // SPECTRUMWIDGET_H
