// AIO Audio: a cleaner player for Kobo's .mp3z audiobooks (libmp3z.so, firmware 4.x).
//
// Stock libmp3z shows a debug screen: English labels ("Backword", "File Name: 001 - x.mp3"), empty
// Forward/Backword slots, linear and log10 volume rows, no elapsed time and, with the GStreamer backend,
// no cover (AudioPlayer::broadcastMetadata only emits trackImageChanged when Qt reports metadata).
//
// Every hook below is a PLT entry of libmp3z.so (verified on 4.45.23697 with REA/Ghidra and the .rel.plt
// table), so only calls made by libmp3z are affected:
//   MP3Reader::MP3Reader(QWidget*, PluginState*)   called by MP3ReaderPlugin::reader: restyle the Ui_MP3Reader form
//   QLabel::setText                                setupUi, applyVolume, setPage and the play/pause lambda: Spanish text
//   AudioPlayer::setMedia(int, QMediaContent, QIODevice*)  setPage: keep the player, read the cover from the QBuffer
//   QLabel::setPixmap                              stock cover lambda: never replaces our cover
// All hooks are optional: if a firmware changes them, Nickel keeps the stock player.

#include <QBoxLayout>
#include <QBuffer>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QMetaMethod>
#include <QPixmap>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <string.h>

#include <NickelHook.h>

#include "aioaudio.h"

#define AIO_DIR "/mnt/onboard/.adds/aio-audio"

static void (*MP3Reader_MP3Reader)(void *self, QWidget *parent, void *state);
static void (*QLabel_setText)(QLabel *self, const QString &text);
static void (*QLabel_setPixmap)(QLabel *self, const QPixmap &pixmap);
static void (*AudioPlayer_setMedia)(void *self, int index, const void *content, QIODevice *device);

static QPointer<AioAudio> aio;

static const char *const BUTTONS[] = {
    "playPause", "backward", "forward", "slower", "normal", "faster",
    "prevChapter", "nextChapter", "logVolumeDown", "logVolumeUp", "volumeDown", "volumeUp", nullptr,
};

template <class T> static T *child(QWidget *root, const char *name) {
    return root->findChild<T *>(QLatin1String(name));
}

static QString clock(qint64 ms) {
    qint64 s = ms > 0 ? ms / 1000 : 0;
    if (s >= 3600)
        return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

// "File Name: 007 - Capitulo 3 (2-4).mp3" -> "Capitulo 3 · parte 2 de 4" (names made by Kobo AIO, others cleaned up).
static QString trackTitle(QString t) {
    t = t.mid(11).trimmed();
    int dot = t.lastIndexOf('.');
    if (dot > 0 && t.length() - dot <= 5)
        t.truncate(dot);
    int i = 0;
    while (i < t.length() && t.at(i).isDigit())
        i++;
    if (i > 0 && i < t.length()) {
        QString rest = t.mid(i);
        while (!rest.isEmpty() && (rest.at(0) == ' ' || rest.at(0) == '-' || rest.at(0) == '_' || rest.at(0) == '.'))
            rest.remove(0, 1);
        if (!rest.isEmpty())
            t = rest;
    }
    if (t.endsWith(')')) {
        int open = t.lastIndexOf('(');
        QStringList kn = t.mid(open + 1, t.length() - open - 2).split('-');
        bool ok1 = false, ok2 = false;
        if (open > 0 && kn.size() == 2) {
            int k = kn[0].toInt(&ok1), n = kn[1].toInt(&ok2);
            if (ok1 && ok2 && k > 0 && n >= k)
                t = QString::fromUtf8("%1 · parte %2 de %3").arg(t.left(open).trimmed()).arg(k).arg(n);
        }
    }
    return t.replace('_', ' ');
}

static QString translate(QLabel *label, const QString &text) {
    const QString name = label->objectName();
    if (name == "playPause") {
        if (text == "Play")  return QString::fromUtf8("Reproducir");
        if (text == "Pause") return QString::fromUtf8("Pausa");
        return text;
    }
    if (name == "title")
        return text.startsWith("File Name: ") ? trackTitle(text) : text;
    if (name == "volume") {
        if (text.startsWith("Volume: "))
            return QString::fromUtf8("Volumen ") + text.mid(8);
        return text;
    }
    static const struct { const char *name, *text; } fixed[] = {
        {"backward", "« 30 s"}, {"forward", "30 s »"},
        {"slower", "0,8×"}, {"normal", "1×"}, {"faster", "1,2×"},
        {"prevChapter", "« Anterior"}, {"nextChapter", "Siguiente »"},
        {"logVolumeDown", "Volumen −"}, {"logVolumeUp", "Volumen +"},
        {"volumeDown", "Volumen −"}, {"volumeUp", "Volumen +"},
        {"total", ""}, {"current", ""},
    };
    for (const auto &f : fixed)
        if (name == QLatin1String(f.name))
            return QString::fromUtf8(f.text);
    return text;
}

// Connect a Nickel TouchLabel's tapped(...) signal (found through its metaobject, so no libnickel headers).
static bool onTap(QObject *button, QObject *receiver, const char *slot) {
    if (!button)
        return false;
    const QMetaObject *mo = button->metaObject();
    int best = -1;
    for (int i = 0; i < mo->methodCount(); i++) {
        QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Signal && m.name() == "tapped" &&
            (best < 0 || m.parameterCount() < mo->method(best).parameterCount()))
            best = i;
    }
    int s = receiver->metaObject()->indexOfSlot(slot);
    if (best < 0 || s < 0)
        return false;
    return QObject::connect(button, mo->method(best), receiver, receiver->metaObject()->method(s));
}

static QByteArray apic(const QByteArray &mp3) {
    const unsigned char *d = reinterpret_cast<const unsigned char *>(mp3.constData());
    int len = mp3.size();
    if (len < 10 || memcmp(d, "ID3", 3) || d[3] < 3 || d[3] > 4 || (d[5] & 0x80)) // unsynchronised tags: skip
        return QByteArray();
    bool v4 = d[3] == 4;
    int end = 10 + ((d[6] & 0x7f) << 21 | (d[7] & 0x7f) << 14 | (d[8] & 0x7f) << 7 | (d[9] & 0x7f));
    int p = 10;
    if (d[5] & 0x40 && p + 4 <= len) // extended header
        p += v4 ? ((d[p] & 0x7f) << 21 | (d[p + 1] & 0x7f) << 14 | (d[p + 2] & 0x7f) << 7 | (d[p + 3] & 0x7f))
                : (d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3]) + 4;
    if (end > len)
        end = len;
    while (p + 10 <= end && d[p]) {
        int size = v4 ? ((d[p + 4] & 0x7f) << 21 | (d[p + 5] & 0x7f) << 14 | (d[p + 6] & 0x7f) << 7 | (d[p + 7] & 0x7f))
                      : (d[p + 4] << 24 | d[p + 5] << 16 | d[p + 6] << 8 | d[p + 7]);
        if (size <= 0 || p + 10 + size > end)
            break;
        if (!memcmp(d + p, "APIC", 4)) {
            const unsigned char *f = d + p + 10;
            int i = 1, n = size, enc = f[0];
            while (i < n && f[i]) i++;          // MIME type
            i += 2;                              // NUL + picture type
            if (enc == 1 || enc == 2) {          // UTF-16 description, 2-byte NUL
                while (i + 1 < n && (f[i] || f[i + 1])) i += 2;
                i += 2;
            } else {
                while (i < n && f[i]) i++;
                i += 1;
            }
            if (i < n)
                return QByteArray(reinterpret_cast<const char *>(f + i), n - i);
            break;
        }
        p += 10 + size;
    }
    return QByteArray();
}

AioAudio::AioAudio(QWidget *r) : QObject(r), reader(r), timer(new QTimer(this)), coverKey(0) {
    QScreen *screen = QGuiApplication::primaryScreen();
    QSize s = screen ? screen->size() : QSize(1072, 1448);
    int w = qMin(s.width(), s.height()), h = qMax(s.width(), s.height());
    int base = qBound(20, h / 48, 44); // 30 px on a 1448 px Clara, 39 px on a 1872 px Libra
    coverBox = QSize(w * 3 / 4, h * 2 / 5);

    current = child<QLabel>(r, "current");
    cover = child<QLabel>(r, "coverImage");

    // Rows: [« 30 s] [Reproducir] [30 s »] / [0,8×] [1×] [1,2×] / [« Anterior] [Siguiente »] / [Volumen −] [Volumen +]
    QBoxLayout *row1 = child<QBoxLayout>(r, "horizontalLayout");
    QBoxLayout *row2 = child<QBoxLayout>(r, "horizontalLayout2");
    QBoxLayout *row3 = child<QBoxLayout>(r, "horizontalLayout3");
    QWidget *back = child<QWidget>(r, "backward"), *fwd = child<QWidget>(r, "forward");
    QWidget *prev = child<QWidget>(r, "prevChapter");
    if (row1 && row2 && back && fwd && child<QWidget>(r, "playPause")) {
        row2->removeWidget(back);
        row2->removeWidget(fwd);
        row1->insertWidget(0, back, 1);
        row1->addWidget(fwd, 1);
    }
    if (row3 && prev) {
        row3->removeWidget(prev);
        row3->insertWidget(0, prev, 1);
    }

    // Linear volume duplicates the log10 row; "Total:" is replaced by the elapsed/total line.
    static const char *const hidden[] = {"volumeUp", "volumeDown", "total", nullptr};
    for (int i = 0; hidden[i]; i++)
        if (QWidget *x = child<QWidget>(r, hidden[i]))
            x->hide();

    for (int i = 0; BUTTONS[i]; i++)
        if (QLabel *b = child<QLabel>(r, BUTTONS[i])) {
            b->setAlignment(Qt::AlignCenter);
            b->setMinimumHeight(base * 5 / 2);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
    static const char *const centered[] = {"title", "volume", "current", nullptr};
    for (int i = 0; centered[i]; i++)
        if (QLabel *l = child<QLabel>(r, centered[i]))
            l->setAlignment(Qt::AlignCenter);
    if (QLabel *t = child<QLabel>(r, "title"))
        t->setWordWrap(true);
    if (cover) {
        cover->setAlignment(Qt::AlignCenter);
        cover->setMinimumSize(1, coverBox.height());
        cover->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    // E-ink friendly: black on white, solid outlines, no gradients.
    r->setStyleSheet(QString(
        "#title { font-size: %1px; font-weight: bold; padding: %3px; }"
        "#current { font-size: %2px; padding: %3px; }"
        "#volume { font-size: %4px; }"
        "#playPause, #backward, #forward, #slower, #normal, #faster, #prevChapter, #nextChapter,"
        "#logVolumeDown, #logVolumeUp, #volumeDown, #volumeUp {"
        " font-size: %2px; color: black; background: white; border: 2px solid black;"
        " border-radius: %3px; margin: %5px; padding: 0 %3px; }"
        "#playPause { font-weight: bold; border-width: 3px; }")
        .arg(base * 4 / 3).arg(base).arg(base / 3).arg(base * 5 / 6).arg(base / 5));

    onTap(back, this, "back()");
    onTap(fwd, this, "ahead()");

    timer->setInterval(5000); // one short e-ink update every 5 s while the reader is open
    connect(timer, SIGNAL(timeout()), this, SLOT(tick()));
    timer->start();
}

void AioAudio::setPlayer(QObject *p) {
    if (p && p->inherits("QMediaPlayer"))
        player = p;
}

void AioAudio::setCover(const QByteArray &mp3) {
    if (!cover)
        return;
    QByteArray jpeg = apic(mp3);
    if (jpeg.isEmpty())
        return;
    int key = qHash(jpeg);
    if (key == coverKey && cover->property("aioCover").toBool())
        return;
    QImage img;
    if (!img.loadFromData(jpeg))
        return;
    QSize box = cover->width() > 1 && cover->height() > 1 ? cover->size().boundedTo(coverBox) : coverBox;
    cover->setPixmap(QPixmap::fromImage(img.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    cover->setProperty("aioCover", true);
    coverKey = key;
}

void AioAudio::tick() {
    if (!player || !current || !reader || !reader->isVisible())
        return;
    qint64 pos = player->property("position").toLongLong();
    qint64 dur = player->property("duration").toLongLong();
    QString text = dur > 0 ? clock(pos) + " / " + clock(dur) : QString();
    if (current->text() != text)
        current->setText(text);
}

void AioAudio::seek(qint64 delta) {
    if (!player)
        return;
    qint64 pos = player->property("position").toLongLong() + delta;
    qint64 dur = player->property("duration").toLongLong();
    if (dur > 0 && pos > dur - 1000)
        pos = dur - 1000;
    if (pos < 0)
        pos = 0;
    QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, pos));
    QTimer::singleShot(400, this, SLOT(tick()));
}

void AioAudio::back() { seek(-30000); }
void AioAudio::ahead() { seek(30000); }

// -- hooks ----------------------------------------------------------------------------------------------

extern "C" __attribute__((visibility("default"))) void _aio_reader(void *self, QWidget *parent, void *state) {
    MP3Reader_MP3Reader(self, parent, state);
    // MP3Reader passes `this` to Ui_MP3Reader::setupUi as the form QWidget, so the QObject is at offset 0.
    QWidget *reader = reinterpret_cast<QWidget *>(self);
    if (qstrcmp(reader->metaObject()->className(), "MP3Reader") || reader->objectName() != "MP3Reader") {
        nh_log("unexpected reader %s, keeping the stock layout", reader->metaObject()->className());
        return;
    }
    aio = new AioAudio(reader);
}

extern "C" __attribute__((visibility("default"))) void _aio_setText(QLabel *self, const QString &text) {
    QLabel_setText(self, self ? translate(self, text) : text);
}

extern "C" __attribute__((visibility("default"))) void _aio_setPixmap(QLabel *self, const QPixmap &pixmap) {
    if (self && self->property("aioCover").toBool())
        return; // our cover, read from the track itself, stays
    QLabel_setPixmap(self, pixmap);
}

extern "C" __attribute__((visibility("default"))) void _aio_setMedia(void *self, int index, const void *content, QIODevice *device) {
    AudioPlayer_setMedia(self, index, content, device);
    if (!aio)
        return;
    aio->setPlayer(reinterpret_cast<QObject *>(self)); // AudioPlayer : QMediaPlayer (single inheritance)
    if (QBuffer *buffer = qobject_cast<QBuffer *>(device))
        aio->setCover(buffer->data());
    aio->tick();
}

static int aio_init() {
    nh_log("AIO Audio ready");
    return 0;
}

static struct nh_info AioAudioInfo = {
    .name            = "AIO Audio",
    .desc            = "Reproductor de audiolibros .mp3z más claro (Kobo AIO)",
    .uninstall_flag  = AIO_DIR "/uninstall",
    .uninstall_xflag = NULL,
    .failsafe_delay  = 3,
};

#define MP3Z "/usr/local/Kobo/libmp3z.so"
static struct nh_hook AioAudioHook[] = {
    {.sym = "_ZN9MP3ReaderC1EP7QWidgetP11PluginState", .sym_new = "_aio_reader", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_MP3Reader), .desc = "player layout", .optional = true},
    {.sym = "_ZN6QLabel7setTextERK7QString", .sym_new = "_aio_setText", .lib = MP3Z, .out = nh_symoutptr(QLabel_setText), .desc = "Spanish labels", .optional = true},
    {.sym = "_ZN6QLabel9setPixmapERK7QPixmap", .sym_new = "_aio_setPixmap", .lib = MP3Z, .out = nh_symoutptr(QLabel_setPixmap), .desc = "keep cover", .optional = true},
    {.sym = "_ZN11AudioPlayer8setMediaEiRK13QMediaContentP9QIODevice", .sym_new = "_aio_setMedia", .lib = MP3Z, .out = nh_symoutptr(AudioPlayer_setMedia), .desc = "cover and seeking", .optional = true},
    {0},
};

static struct nh_dlsym AioAudioDlsym[] = {
    {0},
};

NickelHook(
    .init  = aio_init,
    .info  = &AioAudioInfo,
    .hook  = AioAudioHook,
    .dlsym = AioAudioDlsym,
)
