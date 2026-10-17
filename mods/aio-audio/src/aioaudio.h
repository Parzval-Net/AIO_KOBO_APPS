#ifndef AIOAUDIO_H
#define AIOAUDIO_H

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QSize>

class QLabel;
class QTimer;
class QWidget;

// One per open .mp3z (child of the MP3Reader widget, so it dies with it).
class AioAudio : public QObject {
    Q_OBJECT
public:
    explicit AioAudio(QWidget *reader);
    void setPlayer(QObject *player);
    void setCover(const QByteArray &mp3);

public Q_SLOTS:
    void back();
    void ahead();
    void tick();

private:
    void seek(qint64 delta);
    QPointer<QWidget> reader;
    QPointer<QObject> player;
    QPointer<QLabel> current;
    QPointer<QLabel> cover;
    QTimer *timer;
    QSize coverBox; // cover area from the screen size
    int coverKey;
};

#endif
