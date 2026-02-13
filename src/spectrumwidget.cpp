#include "spectrumwidget.h"
#include <QPainter>
#include <QLinearGradient>
#include <QtMath>

SpectrumWidget::SpectrumWidget(QWidget *parent)
    : QWidget(parent)
{
    m_magnitudes.fill(0.0f, NUM_BANDS);
    setMinimumSize(300, 200);
}

void SpectrumWidget::setSpectrumData(const QVector<float>& magnitudes)
{
    if (magnitudes.size() != NUM_BANDS) return;

    // Apply temporal smoothing to prevent jittery bars
    for (int i = 0; i < NUM_BANDS; ++i) {
        m_magnitudes[i] = m_magnitudes[i] * 0.7f + magnitudes[i] * 0.3f;
    }

    update();
}

void SpectrumWidget::clear()
{
    m_magnitudes.fill(0.0f);
    update();
}

void SpectrumWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();
    const int numBars = m_magnitudes.size();
    const int totalBarWidth = numBars * (BAR_WIDTH + BAR_GAP) - BAR_GAP;
    const int offsetX = (w - totalBarWidth) / 2;

    // Background gradient
    QLinearGradient bgGradient(0, 0, 0, h);
    bgGradient.setColorAt(0, QColor(10, 10, 10));
    bgGradient.setColorAt(1, QColor(30, 30, 30));
    painter.fillRect(rect(), bgGradient);

    // Draw spectrum bars
    for (int i = 0; i < numBars; ++i) {
        float magnitude = m_magnitudes[i];
        int barHeight = static_cast<int>(magnitude * (h - 20));
        int x = offsetX + i * (BAR_WIDTH + BAR_GAP);
        int y = h - barHeight - 10;

        // Color gradient based on magnitude
        QColor barColor = getColorForMagnitude(magnitude);

        // Draw main bar with gradient
        QLinearGradient barGradient(x, y + barHeight, x, y);
        barGradient.setColorAt(0, barColor.darker(150));
        barGradient.setColorAt(1, barColor);
        painter.fillRect(x, y, BAR_WIDTH, barHeight, barGradient);

        // Peak cap (bright line at top)
        if (barHeight > 0) {
            painter.fillRect(x, y - 2, BAR_WIDTH, 2, barColor.lighter(150));
        }
    }
}

QColor SpectrumWidget::getColorForMagnitude(float mag)
{
    // Blue → Cyan → Green → Yellow → Red gradient
    if (mag < 0.2f) return QColor(52, 152, 219);      // Blue
    if (mag < 0.4f) return QColor(26, 188, 156);      // Cyan
    if (mag < 0.6f) return QColor(46, 204, 113);      // Green
    if (mag < 0.8f) return QColor(241, 196, 15);      // Yellow
    return QColor(231, 76, 60);                        // Red
}
