// AIO Audio: Kobo's .mp3z audiobook player (libmp3z.so, firmware 4.x) restyled like Nickel's native one.
//
// Stock libmp3z shows a debug screen: English labels ("Backword", "File Name: 001 - x.mp3"), empty
// Forward/Backword slots, linear and log10 volume rows, no elapsed time and, with the GStreamer backend,
// no cover (AudioPlayer::broadcastMetadata only emits trackImageChanged when Qt reports metadata).
//
// Layout built on top of the stock Ui_MP3Reader form (portrait, black on white, sizes scaled from a 1680 px
// tall 300 ppi screen, every touch target >= 9 mm):
//   header      [<-] Regresar a Mis libros            [timer] 15 min  1,2x  [vol] - 10 +
//               (stock slower/faster/normal/logVolume* buttons re-wired; thin black line below)
//   cover       square box like Nickel's (550 px on 1680 px), cover read from the track's ID3 APIC,
//               book title (ID3 TALB) under it
//   transport   [-30]  [play/pause]  [+30]  (Nickel's audiobook icons, painted fallbacks)
//   times       elapsed ............ total, and a seek slider (tap or drag)
//   (stretch)
//   chapter     [toc] chapter name            [<] [>]  [bluetooth]
//   footer      whole-book bar and "Pista 7 de 15 · 37% del libro · quedan ≈ 3 h 20 min"
// Extras: sleep timer (15/30/60 min or end of track), resume inside the track (positions.ini: libmp3z only
// remembers the track), inverted 150 ms tap feedback, and e-ink care (5 s updates only while playing, only
// labels that changed are touched).
//
// Every hook below is a PLT entry of libmp3z.so (verified on 4.45.23697 with REA/Ghidra and the .rel.plt
// table), so only calls made by libmp3z are affected:
//   MP3Reader::MP3Reader(QWidget*, PluginState*)   called by MP3ReaderPlugin::reader: restyle the Ui_MP3Reader form
//   QLabel::setText                                setupUi, applyVolume, setPage and the play/pause lambda
//   AudioPlayer::setMedia(int, QMediaContent, QIODevice*)  setPage: keep the player, read tags and cover
//   QLabel::setPixmap, QWidget::setGeometry        stock cover lambda: never replaces or moves our cover
// Optional symbols: TouchLabel::set(De)SelectedPixmap and ReadingController::closeReadingView (libnickel,
// nh_dlsym) and MP3Reader::saveBookmark (libmp3z is RTLD_LOCAL: dlopen RTLD_NOLOAD + dlsym at runtime).
// All hooks and symbols are optional: if a firmware changes them, Nickel keeps the stock player.

#include <QBoxLayout>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLayout>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QRegExp>
#include <QScreen>
#include <QSettings>
#include <QSpacerItem>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QTouchEvent>
#include <QVariant>
#include <QWidget>
#include <qmath.h>

#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>

#include <NickelHook.h>

#include "aioaudio.h"

#define AIO_DIR "/mnt/onboard/.adds/aio-audio"
#define MP3Z "/usr/local/Kobo/libmp3z.so"
#define MAX_TRACKS 999 // TRCK totals and saved track keys above this are treated as garbage

static void (*MP3Reader_MP3Reader)(void *self, QWidget *parent, void *state);
static void (*QLabel_setText)(QLabel *self, const QString &text);
static void (*QLabel_setPixmap)(QLabel *self, const QPixmap &pixmap);
static void (*QWidget_setGeometry)(QWidget *self, const QRect &rect);
static void (*AudioPlayer_setMedia)(void *self, int index, const void *content, QIODevice *device);
static void (*TouchLabel_setDeselectedPixmap)(QLabel *self, const QPixmap &pixmap); // libnickel, optional
static void (*TouchLabel_setSelectedPixmap)(QLabel *self, const QPixmap &pixmap);   // libnickel, optional
static void (*ReadingController_closeReadingView)();                                 // libnickel, optional
static void (*MP3Reader_saveBookmark)(void *self);                                   // libmp3z, optional
// Bluetooth output (libnickel, optional). Kobo's own AudiobookPlayer::updateSpeaker pushes a volume to the A2DP
// device before playing; libmp3z never does, so mtkbtd's PCM keeps Volume=0 and the headset stays silent.
static void *(*BluetoothManager_sharedInstance)();
static QString (*BluetoothManager_a2dpSink)(const void *self);                        // "/org/bluez/hci0/dev_..."
static bool (*BluetoothManager_supportsAbsoluteVolume)(void *self, const QString &dev);
static void (*BluetoothManager_setSoftwareVolume)(void *self, const QString &dev, bool software);
static void (*BluetoothManager_setMuted)(void *self, const QString &dev, bool left, bool right);
static void (*BluetoothManager_setVolume)(void *self, const QString &dev, unsigned char left, unsigned char right);

static int btVolume = -1; // 0..127, ours (positions.ini "btVolume"), -1 = not loaded yet

// The user partition is mounted (not exported over USB): /mnt/onboard is its own filesystem and our folder,
// which KoboRoot always ships (LEEME.txt), is there. Otherwise a QSettings write would land on the rootfs.
static bool onboardReady() {
    struct stat onboard, mnt;
    if (stat("/mnt/onboard", &onboard) != 0 || stat("/mnt", &mnt) != 0 || onboard.st_dev == mnt.st_dev)
        return false;
    return QDir(QLatin1String(AIO_DIR)).exists();
}

// The headset volume API is complete: only then the volume buttons and the stream gain are ours.
static bool btVolumeApi() {
    return BluetoothManager_sharedInstance && BluetoothManager_a2dpSink && BluetoothManager_setVolume;
}

static int btVolumeLoad() {
    if (btVolume < 0) {
        QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
        btVolume = qBound(0, s.value("btVolume", 80).toInt(), 127);
    }
    return btVolume;
}

// Send our volume to the connected A2DP headset. Returns false when there is no headset (or no API).
static bool btVolumeApply() {
    if (!BluetoothManager_sharedInstance || !BluetoothManager_a2dpSink || !BluetoothManager_setVolume)
        return false;
    void *bm = BluetoothManager_sharedInstance();
    if (!bm)
        return false;
    const QString dev = BluetoothManager_a2dpSink(bm);
    if (dev.isEmpty()) {
        nh_log("AIO Audio: no A2DP headset connected");
        return false;
    }
    const bool absolute = BluetoothManager_supportsAbsoluteVolume ? BluetoothManager_supportsAbsoluteVolume(bm, dev) : true;
    if (BluetoothManager_setSoftwareVolume)
        BluetoothManager_setSoftwareVolume(bm, dev, !absolute);
    if (BluetoothManager_setMuted)
        BluetoothManager_setMuted(bm, dev, false, false);
    const unsigned char v = (unsigned char)btVolumeLoad();
    BluetoothManager_setVolume(bm, dev, v, v);
    nh_log("AIO Audio: headset volume %d/127 (%s)", v, absolute ? "absolute" : "software");
    return true;
}

static int volumePercent() {
    return (btVolumeLoad() * 100 + 63) / 127;
}

// Full stream gain and our level on the headset; with no headset, our level as the stream gain instead.
// Without the Bluetooth API the stock log10 stream volume is left alone.
static void applyVolumeTo(QObject *player) {
    if (!btVolumeApi())
        return;
    const bool headset = btVolumeApply();
    if (player)
        QMetaObject::invokeMethod(player, "setVolume", Q_ARG(int, headset ? 100 : volumePercent()));
}

// The AudioPlayer of an MP3Reader (MP3Reader+0x40 on 4.45, RE).
static QObject *readerPlayer(const QWidget *reader) {
    return *reinterpret_cast<QObject *const *>(reinterpret_cast<const char *>(reader) + 0x40);
}

static QPointer<AioAudio> aio;

static const qreal RATES[] = {1.0, 1.2, 1.5, 0.8};
static const char *const RATE_NAMES[] = {"1x", "1,2x", "1,5x", "0,8x"};
static const int SLEEP_MIN[] = {0, 15, 30, 60};

// libmp3z is loaded RTLD_LOCAL by Nickel's plugin loader, so RTLD_DEFAULT may not see its symbols.
static void resolveMp3z() {
    static bool done = false;
    if (done)
        return;
    done = true;
    static const char sym[] = "_ZN9MP3Reader12saveBookmarkEv";
    if (void *h = dlopen(MP3Z, RTLD_LAZY | RTLD_NOLOAD)) {
        MP3Reader_saveBookmark = reinterpret_cast<void (*)(void *)>(dlsym(h, sym));
        dlclose(h); // only drops the reference NOLOAD added: the plugin keeps the library loaded
    }
    if (!MP3Reader_saveBookmark)
        MP3Reader_saveBookmark = reinterpret_cast<void (*)(void *)>(dlsym(RTLD_DEFAULT, sym));
    if (!MP3Reader_saveBookmark)
        nh_log("MP3Reader::saveBookmark not found, closing without it");
}

template <class T> static T *child(QWidget *root, const char *name) {
    return root->findChild<T *>(QLatin1String(name));
}

// Take a widget out of whatever stock layout holds it (removeWidget is a no-op elsewhere).
static void detach(QWidget *root, QWidget *w) {
    if (!w)
        return;
    QList<QLayout *> ls = root->findChildren<QLayout *>();
    for (int i = 0; i < ls.size(); i++)
        ls.at(i)->removeWidget(w);
}

static void repolish(QWidget *w) {
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

static QString hms(qint64 ms) {
    qint64 s = ms > 0 ? ms / 1000 : 0;
    if (s >= 3600)
        return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

static QString span(qint64 ms) {
    qint64 min = (qMax<qint64>(0, ms) + 59999) / 60000;
    if (min >= 60)
        return QString("%1 h %2 min").arg(min / 60).arg(min % 60);
    return QString("%1 min").arg(min);
}

// Names that only number a part ("Parte 1", "CD 2"), as Kobo AIO's audiobooks.py _PART_DIR.
static bool isPartName(const QString &s) {
    static const QRegExp re(QLatin1String("^(parte?|part|cd|dis[ck]o?|volumen|vol\\.?|tomo)\\s*\\d+$"), Qt::CaseInsensitive);
    return re.exactMatch(s);
}

// "File Name: 007 - Capitulo 3 (2-4).mp3" -> "Capitulo 3 · parte 2 de 4", "Parte 1 (3-15)" -> "Parte 1 · 3/15"
// (entries made by Kobo AIO: "NNN - chapter (part-parts).mp3"; others are cleaned up).
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
    t.replace('_', ' ');
    if (t.endsWith(')')) {
        int open = t.lastIndexOf('(');
        QStringList kn = t.mid(open + 1, t.length() - open - 2).split('-');
        bool ok1 = false, ok2 = false;
        if (open > 0 && kn.size() == 2) {
            int k = kn[0].toInt(&ok1), n = kn[1].toInt(&ok2);
            if (ok1 && ok2 && k > 0 && n >= k) {
                QString name = t.left(open).trimmed();
                // one arg() pass: no %N from the name
                t = QString::fromUtf8(isPartName(name) ? "%1 · %2/%3" : "%1 · parte %2 de %3")
                        .arg(name, QString::number(k), QString::number(n));
            }
        }
    }
    return t;
}

static QString translate(QLabel *label, const QString &text, bool compact) {
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
            return compact ? text.mid(8) : QString::fromUtf8("Volumen ") + text.mid(8);
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

// Drop the stock connections from a button's tapped(...) to `receiver` (MP3Reader::slower/normal/faster).
static void unwire(QObject *button, QObject *receiver) {
    if (!button || !receiver)
        return;
    const QMetaObject *mo = button->metaObject();
    for (int i = 0; i < mo->methodCount(); i++) {
        QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Signal && m.name() == "tapped")
            QObject::disconnect(button, m, receiver, QMetaMethod());
    }
}

// -- ID3v2 ------------------------------------------------------------------------------------------------

static quint32 be32(const unsigned char *b) {
    return quint32(b[0]) << 24 | quint32(b[1]) << 16 | quint32(b[2]) << 8 | quint32(b[3]);
}
static quint32 syncsafe(const unsigned char *b) {
    return quint32(b[0] & 0x7f) << 21 | quint32(b[1] & 0x7f) << 14 | quint32(b[2] & 0x7f) << 7 | quint32(b[3] & 0x7f);
}

// The payload of the first `id` frame of an ID3v2.3/2.4 tag (no copy: valid while `mp3` is). Every size is
// checked against what is left, so a damaged tag only means "no frame".
static QByteArray id3Frame(const QByteArray &mp3, const char *id) {
    const unsigned char *d = reinterpret_cast<const unsigned char *>(mp3.constData());
    const quint32 len = quint32(mp3.size());
    if (len < 10 || memcmp(d, "ID3", 3) || d[3] < 3 || d[3] > 4 || (d[5] & 0x80)) // unsynchronised tags: skip
        return QByteArray();
    const bool v4 = d[3] == 4;
    quint32 end = qMin(len, 10 + syncsafe(d + 6));
    quint32 p = 10;
    if (d[5] & 0x40) { // extended header
        if (end - p < 4)
            return QByteArray();
        quint32 ext = v4 ? syncsafe(d + p) : be32(d + p);
        if (!v4 && ext <= 0xfffffff0u)
            ext += 4; // v2.3 does not count its own size field
        if (ext > end - p)
            return QByteArray();
        p += ext;
    }
    while (end - p >= 10 && d[p]) {
        quint32 size = v4 ? syncsafe(d + p + 4) : be32(d + p + 4);
        if (size == 0 || size > end - p - 10)
            break;
        if (!memcmp(d + p, id, 4))
            return QByteArray::fromRawData(reinterpret_cast<const char *>(d + p + 10), int(size));
        p += 10 + size;
    }
    return QByteArray();
}

// The APIC picture data.
static QByteArray apic(const QByteArray &mp3) {
    const QByteArray fr = id3Frame(mp3, "APIC");
    const quint32 n = quint32(fr.size());
    if (n < 1)
        return QByteArray();
    const unsigned char *f = reinterpret_cast<const unsigned char *>(fr.constData());
    quint32 i = 1, enc = f[0];
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
        return QByteArray(reinterpret_cast<const char *>(f + i), int(n - i));
    return QByteArray();
}

// A text frame (TALB, TRCK, ...): Latin-1, UTF-16 with or without BOM, or UTF-8.
static QString id3Text(const QByteArray &mp3, const char *id) {
    const QByteArray fr = id3Frame(mp3, id);
    const int n = fr.size();
    if (n < 2)
        return QString();
    const unsigned char *f = reinterpret_cast<const unsigned char *>(fr.constData());
    const unsigned char enc = f[0];
    QString out;
    if (enc == 1 || enc == 2) {
        int i = 1;
        bool le = false;
        if (enc == 1 && n >= 3) {
            if (f[1] == 0xff && f[2] == 0xfe) { le = true; i = 3; }
            else if (f[1] == 0xfe && f[2] == 0xff) i = 3;
        }
        for (; i + 1 < n; i += 2) {
            ushort c = le ? ushort(f[i] | (f[i + 1] << 8)) : ushort((f[i] << 8) | f[i + 1]);
            if (!c)
                break;
            out.append(QChar(c));
        }
    } else {
        int e = 1;
        while (e < n && f[e])
            e++;
        const char *b = reinterpret_cast<const char *>(f + 1);
        out = enc == 3 ? QString::fromUtf8(b, e - 1) : QString::fromLatin1(b, e - 1);
    }
    return out.trimmed();
}

// -- icons ------------------------------------------------------------------------------------------------

enum Glyph { G_PLAY, G_PAUSE, G_BACK30, G_FWD30, G_ARROW, G_VOLUME, G_TOC, G_BT, G_TIMER, G_KNOB, G_PREV, G_NEXT, G_MINUS, G_PLUS };

// Painted stand-ins (black on transparent) for when a Nickel resource is missing.
static QPixmap glyph(Glyph g, int size) {
    size = qMax(8, size);
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const qreal s = size, lw = qMax<qreal>(2, s / 14);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::black, lw, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        auto P = [s](qreal x, qreal y) { return QPointF(x * s, y * s); };
        const QRectF ring(lw, lw, s - 2 * lw, s - 2 * lw);
        switch (g) {
        case G_PLAY: {
            p.drawEllipse(ring);
            QPolygonF tri;
            tri << P(0.40, 0.30) << P(0.72, 0.50) << P(0.40, 0.70);
            p.setBrush(Qt::black);
            p.drawPolygon(tri);
            break;
        }
        case G_PAUSE:
            p.drawEllipse(ring);
            p.fillRect(QRectF(P(0.36, 0.30), P(0.46, 0.70)), Qt::black);
            p.fillRect(QRectF(P(0.54, 0.30), P(0.64, 0.70)), Qt::black);
            break;
        case G_BACK30:
        case G_FWD30: { // counter-clockwise ring with the arrow head at the top right (mirrored below for +30)
            const qreal r = s / 2 - lw * 1.5, a = 70 * M_PI / 180;
            const QPointF c(s / 2, s / 2);
            p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 110 * 16, 320 * 16);
            const QPointF tip(c.x() + r * qCos(a), c.y() - r * qSin(a));
            const QPointF dir(-qSin(a), -qCos(a)), nrm(-dir.y(), dir.x());
            const qreal hl = s * 0.16;
            QPolygonF head;
            head << tip + dir * hl << tip + nrm * (hl * 0.6) << tip - nrm * (hl * 0.6);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawPolygon(head);
            break;
        }
        case G_ARROW:
            p.drawLine(P(0.80, 0.50), P(0.22, 0.50));
            p.drawLine(P(0.22, 0.50), P(0.46, 0.26));
            p.drawLine(P(0.22, 0.50), P(0.46, 0.74));
            break;
        case G_VOLUME: {
            QPolygonF spk;
            spk << P(0.14, 0.38) << P(0.30, 0.38) << P(0.50, 0.20) << P(0.50, 0.80) << P(0.30, 0.62) << P(0.14, 0.62);
            p.setBrush(Qt::black);
            p.drawPolygon(spk);
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(P(0.38, 0.38), P(0.62, 0.62)), -50 * 16, 100 * 16);
            p.drawArc(QRectF(P(0.18, 0.18), P(0.82, 0.82)), -50 * 16, 100 * 16);
            break;
        }
        case G_TOC:
            for (int i = 0; i < 3; i++) {
                qreal y = 0.28 + 0.22 * i;
                p.drawLine(P(0.38, y), P(0.84, y));
                p.drawPoint(P(0.18, y));
            }
            break;
        case G_BT: {
            QPolygonF b;
            b << P(0.32, 0.32) << P(0.68, 0.66) << P(0.50, 0.84) << P(0.50, 0.16) << P(0.68, 0.34) << P(0.32, 0.68);
            p.drawPolyline(b);
            break;
        }
        case G_TIMER:
            p.drawEllipse(QRectF(P(0.16, 0.22), P(0.84, 0.90)));
            p.drawLine(P(0.50, 0.56), P(0.50, 0.36));
            p.drawLine(P(0.50, 0.56), P(0.64, 0.56));
            p.drawLine(P(0.40, 0.08), P(0.60, 0.08));
            p.drawLine(P(0.50, 0.08), P(0.50, 0.22));
            break;
        case G_KNOB:
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawEllipse(QRectF(P(0.10, 0.10), P(0.90, 0.90)));
            break;
        case G_PREV: {
            QPolygonF c;
            c << P(0.60, 0.22) << P(0.34, 0.50) << P(0.60, 0.78);
            p.drawPolyline(c);
            break;
        }
        case G_NEXT: {
            QPolygonF c;
            c << P(0.40, 0.22) << P(0.66, 0.50) << P(0.40, 0.78);
            p.drawPolyline(c);
            break;
        }
        case G_MINUS:
            p.drawLine(P(0.24, 0.50), P(0.76, 0.50));
            break;
        case G_PLUS:
            p.drawLine(P(0.24, 0.50), P(0.76, 0.50));
            p.drawLine(P(0.50, 0.24), P(0.50, 0.76));
            break;
        }
    }
    if (g == G_BACK30 || g == G_FWD30) {
        if (g == G_FWD30)
            img = img.mirrored(true, false);
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont f;
        f.setPixelSize(qMax(6, int(s * 0.30)));
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, QString("30"));
    }
    return QPixmap::fromImage(img);
}

// A Nickel resource at `size` px tall (Dragon sizes scaled to the screen), or the painted stand-in.
static QPixmap nickelIcon(const char *path, int size, Glyph fallback) {
    QPixmap pm(QString::fromLatin1(path));
    if (pm.isNull() || pm.height() <= 0)
        return glyph(fallback, size);
    if (qAbs(pm.height() - size) * 10 > size)
        pm = pm.scaledToHeight(size, Qt::SmoothTransformation);
    return pm;
}

// Tap feedback: the icon in white on a black box.
static QPixmap inverted(const QPixmap &pm, const QSize &box) {
    QSize sz = box.expandedTo(pm.size());
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.drawPixmap((sz.width() - pm.width()) / 2, (sz.height() - pm.height()) / 2, pm);
    }
    img.invertPixels();
    return QPixmap::fromImage(img);
}

// -- AioTap -----------------------------------------------------------------------------------------------

AioTap::AioTap(QWidget *parent) : QLabel(parent), down(false) {
    setAttribute(Qt::WA_AcceptTouchEvents);
}

// A mouse event this soon after a touch is the copy the platform plugin may also deliver.
static bool recentTouch(const QElapsedTimer &t) {
    return t.isValid() && t.elapsed() < 500;
}

bool AioTap::event(QEvent *e) {
    switch (e->type()) {
    case QEvent::TouchBegin:
        lastTouch.start();
        down = true;
        e->accept();
        return true;
    case QEvent::TouchUpdate:
        lastTouch.start();
        e->accept();
        return true;
    case QEvent::TouchEnd: {
        lastTouch.start();
        const QTouchEvent *te = static_cast<QTouchEvent *>(e);
        bool inside = te->touchPoints().isEmpty() || rect().contains(te->touchPoints().first().pos().toPoint());
        bool was = down;
        down = false;
        e->accept();
        if (was && inside)
            emit tapped();
        return true;
    }
    case QEvent::TouchCancel:
        lastTouch.start();
        down = false;
        e->accept();
        return true;
    default:
        return QLabel::event(e);
    }
}

void AioTap::mousePressEvent(QMouseEvent *e) {
    e->accept();
    if (recentTouch(lastTouch))
        return;
    down = e->button() == Qt::LeftButton;
}

void AioTap::mouseReleaseEvent(QMouseEvent *e) {
    if (recentTouch(lastTouch)) {
        e->accept();
        return;
    }
    bool was = down;
    down = false;
    e->accept();
    if (was && rect().contains(e->pos()))
        emit tapped();
}

// -- AioSlider --------------------------------------------------------------------------------------------

AioSlider::AioSlider(QWidget *parent, bool i, int t, const QPixmap &k)
    : QWidget(parent), value(0), maximum(0), dragValue(0), interactive(i), dragging(false), thickness(qMax(1, t)),
      drawnX(-1), knob(k) {
    if (interactive)
        setAttribute(Qt::WA_AcceptTouchEvents);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

int AioSlider::margin() const {
    return interactive ? knob.width() / 2 + 1 : 0;
}

int AioSlider::xFor(qint64 v) const {
    const int m = margin(), w = width() - 2 * m;
    if (maximum <= 0 || w <= 0)
        return m;
    return m + int(qBound<qint64>(0, v, maximum) * w / maximum);
}

qint64 AioSlider::valueAt(qreal x) const {
    const int m = margin(), w = width() - 2 * m;
    if (maximum <= 0 || w <= 0)
        return 0;
    return qint64(qBound<qreal>(0, (x - m) / w, 1) * maximum);
}

void AioSlider::setValue(qint64 v, qint64 maxValue) {
    value = v;
    maximum = maxValue;
    if (!dragging && xFor(value) != drawnX)
        update();
}

void AioSlider::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    const int m = margin(), cy = height() / 2, x = xFor(dragging ? dragValue : value);
    drawnX = x;
    p.fillRect(QRect(m, cy - thickness / 2, qMax(0, width() - 2 * m), thickness), QColor(0xbb, 0xbb, 0xbb));
    if (x > m)
        p.fillRect(QRect(m, cy - thickness / 2 - 1, x - m, thickness + 2), Qt::black);
    if (interactive && maximum > 0 && !knob.isNull())
        p.drawPixmap(x - knob.width() / 2, cy - knob.height() / 2, knob);
}

void AioSlider::press(qreal x) {
    if (!interactive || maximum <= 0)
        return;
    dragging = true;
    dragValue = valueAt(x);
    update();
}

void AioSlider::drag(qreal x) {
    if (!dragging)
        return;
    dragValue = valueAt(x);
    if (qAbs(xFor(dragValue) - drawnX) >= qMax(4, width() / 50)) // e-ink: repaint every ~2% of travel
        update();
}

void AioSlider::release(qreal x) {
    if (!dragging)
        return;
    dragValue = valueAt(x);
    dragging = false;
    value = dragValue;
    update();
    emit seekRequested(value);
}

bool AioSlider::event(QEvent *e) {
    switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        lastTouch.start();
        const QTouchEvent *te = static_cast<QTouchEvent *>(e);
        if (!te->touchPoints().isEmpty()) {
            qreal x = te->touchPoints().first().pos().x();
            if (e->type() == QEvent::TouchBegin)
                press(x);
            else if (e->type() == QEvent::TouchUpdate)
                drag(x);
            else
                release(x);
        }
        e->accept();
        return true;
    }
    case QEvent::TouchCancel:
        lastTouch.start();
        dragging = false;
        update();
        e->accept();
        return true;
    default:
        return QWidget::event(e);
    }
}

void AioSlider::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && !recentTouch(lastTouch))
        press(e->pos().x());
    e->accept();
}

void AioSlider::mouseMoveEvent(QMouseEvent *e) {
    if (!recentTouch(lastTouch))
        drag(e->pos().x());
    e->accept();
}

void AioSlider::mouseReleaseEvent(QMouseEvent *e) {
    if (!recentTouch(lastTouch))
        release(e->pos().x());
    e->accept();
}

// -- AioAudio ---------------------------------------------------------------------------------------------

int AioAudio::px(int dragon) const {
    return qMax(1, qRound(dragon * scale));
}

AioAudio::AioAudio(QWidget *r)
    : QObject(r), reader(r), timer(new QTimer(this)), flashTimer(new QTimer(this)), sleepTimer(new QTimer(this)),
      scale(1), coverKey(0), playing(false), started(false), pauseOnPlay(false), track(-1), trackNo(0), trackCount(0),
      lastPos(0), lastDur(0), pendingSeek(-1), resumeTarget(-1), savedTrack(-1), savedPos(0), speedIndex(0), sleepMode(0) {
    resolveMp3z();
    QScreen *screen = QGuiApplication::primaryScreen();
    QSize s = screen ? screen->size() : QSize(1264, 1680);
    const int w = qMin(s.width(), s.height()), h = qMax(s.width(), s.height());
    scale = h / 1680.0;                          // Libra Colour / Libra 2: 1264x1680 at 300 ppi ("Dragon")
    const int side = qMin(px(550), w * 4 / 5);
    coverBox = QSize(side, side);
    const int target = px(106);                  // 9 mm at 300 ppi

    timer->setInterval(5000);                    // e-ink: one short update every 5 s, only while playing
    connect(timer, SIGNAL(timeout()), this, SLOT(tick()));
    stateTimer = new QTimer(this);
    stateTimer->setSingleShot(true);
    stateTimer->setInterval(400);                // play/pause flips settle for 0.4 s before anything repaints
    connect(stateTimer, SIGNAL(timeout()), this, SLOT(applyPlayState()));
    flashTimer->setSingleShot(true);
    flashTimer->setInterval(150);
    connect(flashTimer, SIGNAL(timeout()), this, SLOT(unflash()));
    sleepTimer->setSingleShot(true);
    connect(sleepTimer, SIGNAL(timeout()), this, SLOT(sleepFired()));

    cover = child<QLabel>(r, "coverImage");
    playPause = child<QLabel>(r, "playPause");
    QLabel *back = child<QLabel>(r, "backward"), *fwd = child<QLabel>(r, "forward");
    QLabel *prev = child<QLabel>(r, "prevChapter"), *next = child<QLabel>(r, "nextChapter");
    QLabel *arrow = child<QLabel>(r, "slower"), *backText = child<QLabel>(r, "faster");
    speed = child<QLabel>(r, "normal");
    QLabel *volDown = child<QLabel>(r, "logVolumeDown"), *volUp = child<QLabel>(r, "logVolumeUp");
    QLabel *volume = child<QLabel>(r, "volume"), *title = child<QLabel>(r, "title");
    QBoxLayout *col = child<QBoxLayout>(r, "verticalLayout");
    QBoxLayout *row1 = child<QBoxLayout>(r, "horizontalLayout");
    if (!col || !row1 || !playPause || !cover) {
        nh_log("AIO Audio: Ui_MP3Reader changed, keeping the stock layout");
        playPause = 0; // stock "Play"/"Pause" text stays
        speed = 0;
        cover = 0;     // stock cover lambda keeps the cover (never marked aioCover)
        return;
    }

    // setupUi's vertical spacers (verticalLayout items 0, 4 and 11, before we insert anything): 0 and 4 become
    // small gaps, 11 stays the stretch that pushes the chapter row and the footer to the bottom.
    static const int gaps[] = {0, 4};
    for (int g = 0; g < 2; g++)
        if (QLayoutItem *it = col->itemAt(gaps[g]))
            if (QSpacerItem *sp = it->spacerItem())
                sp->changeSize(0, px(g ? 28 : 20), QSizePolicy::Minimum, QSizePolicy::Fixed);
    QSpacerItem *bottom = col->count() > 0 && col->itemAt(col->count() - 1) ? col->itemAt(col->count() - 1)->spacerItem() : 0;
    if (bottom)
        bottom->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    col->setContentsMargins(px(24), px(4), px(24), px(16));
    col->setSpacing(px(10));

    // Header: [<-] Regresar a Mis libros ........ [timer] 15 min  1,2x  [vol] - 10 +
    QWidget *header = new QWidget(r);
    header->setObjectName("aioHeader");
    header->setFixedHeight(target);
    QHBoxLayout *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(px(12));
    if (arrow && backText && ReadingController_closeReadingView) {
        detach(r, arrow);
        detach(r, backText);
        unwire(arrow, r);    // stock: MP3Reader::slower()
        unwire(backText, r); // stock: MP3Reader::faster()
        arrow->setFixedSize(target, target);
        arrow->setAlignment(Qt::AlignCenter);
        setIcon(arrow, nickelIcon(":/images/reading/back_arrow.png", px(56), G_ARROW), QSize(target, target));
        backText->setText(QString::fromUtf8("Regresar a Mis libros"));
        backText->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        backText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        backText->setFixedHeight(target);
        QLabel *const backs[] = {arrow, backText};
        for (QLabel *l : backs) {
            l->setProperty("aioOwned", true);
            onTap(l, this, "flashSender()");
            onTap(l, this, "goHome()");
        }
        hl->addWidget(arrow);
        hl->addWidget(backText, 1);
    } else {
        if (arrow) arrow->hide();
        if (backText) backText->hide();
        hl->addStretch(1);
    }
    AioTap *sleepIcon = new AioTap(header);
    sleepIcon->setObjectName("aioSleep");
    sleepIcon->setAlignment(Qt::AlignCenter);
    sleepIcon->setFixedSize(px(84), target);
    setIcon(sleepIcon, nickelIcon(":/images/reading/timer.png", px(70), G_TIMER), QSize(px(84), target));
    AioTap *sleepLabel = new AioTap(header);
    sleepLabel->setObjectName("aioSleepText");
    sleepLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    sleepLabel->setFixedHeight(target);
    sleepText = sleepLabel;
    AioTap *const sleeps[] = {sleepIcon, sleepLabel};
    for (AioTap *t : sleeps) {
        connect(t, SIGNAL(tapped()), this, SLOT(flashSender()));
        connect(t, SIGNAL(tapped()), this, SLOT(cycleSleep()));
    }
    hl->addWidget(sleepIcon);
    hl->addWidget(sleepLabel);
    if (speed) {
        detach(r, speed);
        unwire(speed, r); // stock: MP3Reader::normal()
        speed->setProperty("aioOwned", true);
        speed->setText(QString::fromLatin1(RATE_NAMES[0]));
        speed->setAlignment(Qt::AlignCenter);
        speed->setFixedHeight(target);
        speed->setMinimumWidth(px(96));
        onTap(speed, this, "flashSender()");
        onTap(speed, this, "cycleSpeed()");
        hl->addSpacing(px(8));
        hl->addWidget(speed);
    }
    if (volDown && volUp) { // headset volume when the Bluetooth API resolved, else the stock log10 volume
        hl->addSpacing(px(8));
        QLabel *volIcon = new QLabel(header);
        volIcon->setObjectName("aioVolumeIcon");
        volIcon->setAlignment(Qt::AlignCenter);
        volIcon->setPixmap(nickelIcon(":/images/reading/volume.png", px(70), G_VOLUME));
        hl->addWidget(volIcon);
        const QSize vb(px(84), target);
        detach(r, volDown);
        volDown->setFixedSize(vb);
        volDown->setAlignment(Qt::AlignCenter);
        setIcon(volDown, glyph(G_MINUS, px(48)), vb);
        hl->addWidget(volDown);
        if (volume) {
            detach(r, volume);
            if (volume->text().startsWith(QString::fromUtf8("Volumen ")))
                volume->setText(volume->text().mid(8));
            volume->setAlignment(Qt::AlignCenter);
            volume->setMinimumWidth(px(44));
            hl->addWidget(volume);
        }
        detach(r, volUp);
        volUp->setFixedSize(vb);
        volUp->setAlignment(Qt::AlignCenter);
        setIcon(volUp, glyph(G_PLUS, px(48)), vb);
        hl->addWidget(volUp);
        const bool ours = btVolumeApi();
        QLabel *const vols[] = {volDown, volUp};
        for (QLabel *l : vols) {
            l->setProperty("aioOwned", true);
            if (ours)
                QObject::disconnect(l, nullptr, r, nullptr); // stock log10 stream volume: replaced by the headset volume
            onTap(l, this, "flashSender()");
        }
        if (ours) {
            onTap(volDown, this, "volumeDown()");
            onTap(volUp, this, "volumeUp()");
            volumeLabel = volume;
            if (volume)
                volume->setProperty("aioOwned", true); // stock applyVolume would write "Volume: 10" here
            showVolume();
        }
    } else {
        if (volDown) volDown->hide();
        if (volUp) volUp->hide();
        if (volume) volume->hide();
    }
    QLabel *line = new QLabel(r);
    line->setObjectName("aioLine");
    line->setFixedHeight(qMax(2, px(2)));
    col->insertWidget(0, header);
    col->insertWidget(1, line);

    // Cover, then the book title (ID3 TALB) under it.
    cover->setAlignment(Qt::AlignCenter);
    cover->setMinimumWidth(1);
    cover->setFixedHeight(side);
    cover->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QLabel *bookLabel = new QLabel(r);
    bookLabel->setObjectName("aioBook");
    bookLabel->setAlignment(Qt::AlignCenter);
    bookLabel->setWordWrap(true);
    bookLabel->hide();
    book = bookLabel;
    col->insertWidget(col->indexOf(cover) + 1, bookLabel);

    // Transport: [-30] [play/pause] [+30]
    playIcon = nickelIcon(":/images/audiobook/play_button.png", px(158), G_PLAY);
    pauseIcon = nickelIcon(":/images/audiobook/pause_button.png", px(158), G_PAUSE);
    const int pb = qMax(target, px(158) + px(12));
    playBox = QSize(pb, pb);
    playPause->setFixedSize(playBox);
    playPause->setAlignment(Qt::AlignCenter);
    playPause->setProperty("aioOwned", true);
    playing = playPause->text() == QString::fromUtf8("Pausa") || playPause->text() == QLatin1String("Pause");
    setIcon(playPause, playing ? pauseIcon : playIcon, playBox);
    onTap(playPause, this, "flashSender()");
    const int sb = qMax(target, px(100) + px(12));
    if (back && fwd) {
        detach(r, back);
        detach(r, fwd);
        back->setFixedSize(sb, sb);
        fwd->setFixedSize(sb, sb);
        back->setAlignment(Qt::AlignCenter);
        fwd->setAlignment(Qt::AlignCenter);
        setIcon(back, nickelIcon(":/images/audiobook/backward_button.png", px(100), G_BACK30), QSize(sb, sb));
        setIcon(fwd, nickelIcon(":/images/audiobook/forward_button.png", px(100), G_FWD30), QSize(sb, sb));
        back->setProperty("aioOwned", true);
        fwd->setProperty("aioOwned", true);
        onTap(back, this, "flashSender()");
        onTap(fwd, this, "flashSender()");
        onTap(back, this, "rewind()"); // stock: empty slots
        onTap(fwd, this, "ahead()");
        row1->insertWidget(0, back);
        row1->addWidget(fwd);
    }
    row1->insertStretch(0, 1);
    row1->addStretch(1);
    row1->setSpacing(px(64));

    // Times and seek slider, above the bottom stretch.
    QWidget *times = new QWidget(r);
    QHBoxLayout *tl = new QHBoxLayout(times);
    tl->setContentsMargins(0, px(8), 0, 0);
    QLabel *el = new QLabel(times), *tot = new QLabel(times);
    el->setObjectName("aioElapsed");
    tot->setObjectName("aioTotal");
    el->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    tot->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tl->addWidget(el);
    tl->addStretch(1);
    tl->addWidget(tot);
    elapsed = el;
    total = tot;
    QPixmap knob = nickelIcon(":/images/reading/scrubber_handle_off.png", px(56), G_KNOB);
    AioSlider *sl = new AioSlider(r, true, qMax(2, px(3)), knob);
    sl->setFixedHeight(qMax(target * 3 / 4, knob.height() + px(8)));
    connect(sl, SIGNAL(seekRequested(qint64)), this, SLOT(seekTo(qint64)));
    slider = sl;
    int at = bottom ? col->count() - 1 : col->count();
    col->insertWidget(at, times);
    col->insertWidget(at + 1, sl);

    // Chapter row: [toc] chapter name ...... [<] [>] [bluetooth]
    QWidget *crow = new QWidget(r);
    crow->setFixedHeight(target);
    QHBoxLayout *cl = new QHBoxLayout(crow);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(px(10));
    QLabel *toc = new QLabel(crow);
    toc->setPixmap(nickelIcon(":/images/reading/reading_toc.png", px(70), G_TOC));
    cl->addWidget(toc);
    QLabel *chap = new QLabel(crow);
    chap->setObjectName("aioChapter");
    chap->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    chap->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    chapter = chap;
    cl->addWidget(chap, 1);
    if (prev && next) { // stock goToPrevPage/goToNextPage stay wired
        const QSize cb(target, target);
        detach(r, prev);
        detach(r, next);
        prev->setFixedSize(cb);
        next->setFixedSize(cb);
        prev->setAlignment(Qt::AlignCenter);
        next->setAlignment(Qt::AlignCenter);
        setIcon(prev, glyph(G_PREV, px(56)), cb);
        setIcon(next, glyph(G_NEXT, px(56)), cb);
        QLabel *const chevrons[] = {prev, next};
        for (QLabel *l : chevrons) {
            l->setProperty("aioOwned", true);
            onTap(l, this, "flashSender()");
            onTap(l, this, "userSkip()");
            cl->addWidget(l);
        }
    }
    QLabel *bt = new QLabel(crow); // TODO: Bluetooth device name (Nickel's BluetoothManager, not resolved here)
    bt->setPixmap(nickelIcon(":/images/reading/bluetooth.png", px(54), G_BT));
    cl->addSpacing(px(6));
    cl->addWidget(bt);
    col->addWidget(crow);

    // Footer: whole-book bar and text.
    AioSlider *bb = new AioSlider(r, false, qMax(2, px(4)), QPixmap());
    bb->setFixedHeight(px(12));
    bb->hide();
    bookBar = bb;
    QLabel *foot = new QLabel(r);
    foot->setObjectName("aioFooter");
    foot->setAlignment(Qt::AlignCenter);
    footer = foot;
    col->addWidget(bb);
    col->addWidget(foot);

    // Hidden: the stock track label (it feeds the chapter row), "current"/"total", speed presets, linear volume.
    static const char *const hidden[] = {"title", "current", "total", "volumeUp", "volumeDown", nullptr};
    for (int i = 0; hidden[i]; i++)
        if (QWidget *x = child<QWidget>(r, hidden[i]))
            x->hide();
    if (title)
        setChapter(title->property("aioName").toString());
    col->invalidate();

    // E-ink friendly: black on white, no borders, no rounded corners, no grey fills (the slider paints its track).
    static const char *const ids[] = {"slower", "faster", "normal", "logVolumeDown", "logVolumeUp", "volume", "playPause",
                                      "backward", "forward", "prevChapter", "nextChapter", "aioSleep", "aioSleepText", nullptr};
    QStringList plain, down;
    for (int i = 0; ids[i]; i++) {
        plain << QString("#%1").arg(QString::fromLatin1(ids[i]));
        down << QString("#%1[aioDown=\"true\"]").arg(QString::fromLatin1(ids[i]));
    }
    r->setStyleSheet(
        plain.join(", ") + " { border: none; background: white; color: black; margin: 0; padding: 0; }" +
        QString("#faster { font-family: DefaultSansSerif; font-weight: bold; font-size: %1px; padding-left: %2px; }").arg(px(30)).arg(px(4)) +
        QString("#normal, #aioSleepText, #volume { font-family: DefaultSansSerif; font-size: %1px; }").arg(px(28)) +
        "#normal { font-weight: bold; }"
        "#aioLine { background: black; border: none; }" +
        QString("#aioBook { font-family: DefaultSerif; font-weight: bold; font-size: %1px; padding: %2px 0; }").arg(px(34)).arg(px(6)) +
        QString("#aioElapsed, #aioTotal { font-family: DefaultSansSerif; font-size: %1px; }").arg(px(28)) +
        QString("#aioChapter { font-family: DefaultSansSerif; font-style: italic; font-size: %1px; padding-left: %2px; }").arg(px(29)).arg(px(6)) +
        QString("#aioFooter { font-family: DefaultSansSerif; font-size: %1px; }").arg(px(24)) +
        down.join(", ") + " { background: black; color: white; }");

    if (playing)
        timer->start();
}

AioAudio::~AioAudio() {
    saveState(false); // the player may already be stopped or gone: use the last position seen
}

void AioAudio::setIcon(QLabel *l, const QPixmap &pm, const QSize &box) {
    if (!l || pm.isNull())
        return;
    QPixmap inv = inverted(pm, box);
    l->setProperty("aioIcon", QVariant::fromValue(pm));
    l->setProperty("aioInv", QVariant::fromValue(inv));
    l->setPixmap(pm); // straight to QtWidgets: our setPixmap hook only covers libmp3z's PLT
    if (l->inherits("TouchLabel")) {
        if (TouchLabel_setDeselectedPixmap)
            TouchLabel_setDeselectedPixmap(l, pm);
        if (TouchLabel_setSelectedPixmap)
            TouchLabel_setSelectedPixmap(l, inv);
    }
}

void AioAudio::setPlayer(QObject *p) {
    if (!p || !p->inherits("QMediaPlayer") || player.data() == p)
        return;
    player = p;
    connect(p, SIGNAL(durationChanged(qint64)), this, SLOT(onDuration()));
    qreal rate = p->property("playbackRate").toReal();
    int best = 0;
    for (int i = 1; i < 4; i++)
        if (qAbs(RATES[i] - rate) < qAbs(RATES[best] - rate))
            best = i;
    speedIndex = rate > 0 ? best : 0;
    if (speed)
        speed->setText(QString::fromLatin1(RATE_NAMES[speedIndex]));
}

bool AioAudio::owns(QObject *p) const {
    return p && reader && readerPlayer(reader.data()) == p;
}

void AioAudio::media(QObject *p, int index, const QByteArray &mp3, bool hasData) {
    setPlayer(p); // AudioPlayer : QMediaPlayer (single inheritance)
    setTrack(index, mp3);
    if (hasData)
        setCover(mp3);
    tick();
}

QString AioAudio::bookKey() const {
    QString k = bookTitle.trimmed();
    k.replace('/', '_');
    k.replace('\\', '_');
    return k;
}

void AioAudio::loadState() {
    savedTrack = -1;
    savedPos = 0;
    durations.clear();
    const QString key = bookKey();
    if (key.isEmpty())
        return;
    QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
    s.beginGroup(key);
    savedTrack = s.value("track", -1).toInt();
    if (savedTrack < 0 || savedTrack > MAX_TRACKS)
        savedTrack = -1;
    savedPos = s.value("pos", 0).toLongLong();
    QStringList ds = s.value("durations").toString().split(' ', QString::SkipEmptyParts);
    for (int i = 0; i < ds.size(); i++) {
        QStringList kv = ds.at(i).split(':');
        bool ok1 = false, ok2 = false;
        if (kv.size() == 2) {
            int idx = kv.at(0).toInt(&ok1);
            qint64 ms = kv.at(1).toLongLong(&ok2);
            if (ok1 && ok2 && idx >= 0 && idx <= MAX_TRACKS && ms > 0)
                durations.insert(idx, ms);
        }
    }
    s.endGroup();
}

// Only on pause, close and track change (never from the 5 s timer).
void AioAudio::saveState(bool askPlayer) {
    const QString key = bookKey();
    if (key.isEmpty() || track < 0 || pendingSeek >= 0)
        return; // a resume not applied yet must not be overwritten
    if (!onboardReady()) {
        nh_log("AIO Audio: /mnt/onboard not mounted, position not saved");
        return;
    }
    qint64 pos = lastPos, dur = lastDur;
    if (askPlayer && player) {
        pos = player->property("position").toLongLong();
        dur = player->property("duration").toLongLong();
    }
    if (pos < 0 || (dur > 0 && pos > dur - 5000))
        pos = 0;
    QStringList ds;
    for (QMap<int, qint64>::const_iterator it = durations.constBegin(); it != durations.constEnd(); ++it)
        ds << QString("%1:%2").arg(it.key()).arg(it.value());
    QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
    s.beginGroup(key);
    s.setValue("track", track);
    s.setValue("pos", pos);
    s.setValue("durations", ds.join(" "));
    s.endGroup();
    s.sync();
    savedTrack = track;
    savedPos = pos;
}

void AioAudio::setTrack(int index, const QByteArray &mp3) {
    const QString album = id3Text(mp3, "TALB");
    const QStringList tr = id3Text(mp3, "TRCK").split('/'); // Kobo AIO: "7/15" over the whole .mp3z
    int no = tr.size() >= 1 ? tr.at(0).trimmed().toInt() : 0;
    int count = tr.size() >= 2 ? tr.at(1).trimmed().toInt() : 0;
    if (no < 1 || no > MAX_TRACKS || count < 0 || count > MAX_TRACKS || (count > 0 && no > count))
        no = count = 0;
    if (!album.isEmpty() && album != bookTitle) {
        bookTitle = album;
        if (book) {
            book->setText(album);
            book->show();
        }
        loadState();
    }
    const int cur = no > 0 ? no - 1 : index;
    const bool first = !started, changed = started && cur != track;
    started = true;
    track = cur;
    trackNo = no > 0 ? no : (index >= 0 ? index + 1 : 0);
    trackCount = count;
    lastPos = 0;
    lastDur = 0;
    if (first) {
        if (savedTrack == cur && savedPos > 5000) { // libmp3z reopens the track, we bring back the minute
            pendingSeek = savedPos;
            QTimer::singleShot(1000, this, SLOT(applyPending()));
            QTimer::singleShot(2500, this, SLOT(applyPending()));
            QTimer::singleShot(5000, this, SLOT(applyPendingLast()));
        }
    } else if (changed) {
        pendingSeek = -1;
        resumeTarget = -1;
        saveState(false); // new track, from its start
        if (sleepMode == 4 && !(skipClock.isValid() && skipClock.elapsed() < 4000)) {
            pauseOnPlay = true; // "end of track": pause as soon as the next track starts
            QTimer::singleShot(2500, this, SLOT(endOfTrackCheck()));
        }
    }
    if (speedIndex != 0)
        QTimer::singleShot(1500, this, SLOT(applyRate()));
}

void AioAudio::applyPending() {
    if (!player)
        return;
    const qint64 pos = player->property("position").toLongLong();
    if (pendingSeek < 0) {
        // the backend may drop a seek made while it was still loading: repeat it once if we are back at 0
        if (resumeTarget > 15000 && pos < 10000)
            QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, resumeTarget));
        resumeTarget = -1;
        return;
    }
    const qint64 dur = player->property("duration").toLongLong();
    if (dur <= 0)
        return; // not loaded yet: next try, or durationChanged
    if (pendingSeek < dur - 3000) {
        QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, pendingSeek));
        lastPos = pendingSeek;
        resumeTarget = pendingSeek;
    }
    pendingSeek = -1;
    QTimer::singleShot(400, this, SLOT(tick()));
}

// Last retry: if the duration is still unknown, seek anyway, so the resume never blocks saveState for good.
void AioAudio::applyPendingLast() {
    if (pendingSeek < 0 || (player && player->property("duration").toLongLong() > 0)) {
        applyPending();
        return;
    }
    if (player) {
        QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, pendingSeek));
        resumeTarget = pendingSeek; // repeated by applyPending once durationChanged arrives, if it was dropped
    }
    lastPos = pendingSeek; // a close before the next tick keeps the saved minute
    pendingSeek = -1;
    QTimer::singleShot(400, this, SLOT(tick()));
}

void AioAudio::applyRate() {
    if (!player)
        return;
    const qreal want = RATES[speedIndex];
    if (qAbs(player->property("playbackRate").toReal() - want) < 0.01)
        return;
    if (!QMetaObject::invokeMethod(player, "setPlaybackRate", Q_ARG(qreal, want)))
        player->setProperty("playbackRate", QVariant(double(want)));
}

void AioAudio::onDuration() {
    if (!player)
        return;
    const qint64 d = player->property("duration").toLongLong();
    if (d > 0 && track >= 0)
        durations.insert(track, d);
    if (d > 0)
        lastDur = d;
    applyPending();
    tick();
}

bool AioAudio::playPauseText(QLabel *label, const QString &text) {
    if (!playPause || label != playPause.data())
        return false;
    if (text == QLatin1String("Pause"))
        wantPlaying = 1;
    else if (text == QLatin1String("Play"))
        wantPlaying = 0;
    else
        return true; // anything else: keep the icon
    // GStreamer can flip Playing/Paused many times per second when the Bluetooth sink stalls. Each flip used to
    // repaint (colour e-ink), query Bluetooth and write positions.ini, which froze the whole device: settle first.
    if (!stateTimer->isActive())
        stateTimer->start();
    return true;
}

void AioAudio::applyPlayState() {
    if (wantPlaying < 0 || !playPause)
        return;
    const bool now = wantPlaying == 1;
    wantPlaying = -1;
    if (now == playing && iconSet)
        return;
    iconSet = true;
    QLabel *label = playPause.data();
    setIcon(label, now ? pauseIcon : playIcon, playBox);
    const bool was = playing;
    playing = now;
    tick();
    if (playing) {
        timer->start();
        if (pauseOnPlay) {
            pauseOnPlay = false;
            QMetaObject::invokeMethod(this, "sleepFired", Qt::QueuedConnection);
        }
    } else {
        timer->stop(); // e-ink: nothing moves while paused
        if (was)
            saveState(true);
    }
}

void AioAudio::setChapter(const QString &name) {
    if (chapter && chapter->text() != name)
        chapter->setText(name);
}

void AioAudio::setCover(const QByteArray &mp3) {
    if (!cover)
        return;
    QByteArray jpeg = apic(mp3);
    if (jpeg.isEmpty()) {
        const QPixmap *cur = cover->pixmap();
        if (cur && !cur->isNull())
            return;
        // No picture anywhere yet: a plain frame with the book title (the stock lambda may still replace it).
        QPixmap ph(coverBox);
        ph.fill(Qt::white);
        QPainter p(&ph);
        p.setRenderHint(QPainter::Antialiasing);
        const int lw = qMax(2, px(3));
        p.setPen(QPen(Qt::black, lw));
        p.drawRect(QRectF(lw / 2.0, lw / 2.0, coverBox.width() - lw, coverBox.height() - lw));
        QFont f;
        f.setPixelSize(px(40));
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(QPoint(0, 0), coverBox).adjusted(px(40), px(40), -px(40), -px(40)),
                   Qt::AlignCenter | Qt::TextWordWrap, bookTitle.isEmpty() ? QString::fromUtf8("Audiolibro") : bookTitle);
        p.end();
        cover->setPixmap(ph);
        return;
    }
    // setPage runs before the reader is laid out, so the label size is not known yet: always use the box
    // computed from the screen, which is what the layout gives the cover anyway.
    int key = int(qHash(jpeg));
    if (key == coverKey && cover->property("aioCover").toBool())
        return;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::ReadOnly);
    QImageReader jr(&buf);
    QSize native = jr.size();
    if (!native.isValid() || native.width() > 8000 || native.height() > 8000)
        return;
    const QByteArray fmt = jr.format().toLower();
    if (fmt != "jpeg" && fmt != "jpg" && qint64(native.width()) * native.height() > 4000000)
        return; // only JPEG decodes at the scaled size: others would be decoded whole first

    jr.setScaledSize(native.scaled(coverBox, Qt::KeepAspectRatio)); // the JPEG handler decodes at that scale
    QImage img = jr.read();
    if (img.isNull())
        return;
    cover->setProperty("aioCover", true);
    cover->setPixmap(QPixmap::fromImage(img));
    coverKey = key;
}

static void setTextIfChanged(QLabel *l, const QString &t) {
    if (l && l->text() != t)
        l->setText(t);
}

void AioAudio::tick() {
    if (!player || !reader)
        return; // hidden labels do not repaint, so no e-ink cost; the 5 s timer only runs while playing
    qint64 pos = player->property("position").toLongLong();
    const qint64 dur = player->property("duration").toLongLong();
    if (pos < 0)
        pos = 0;
    lastPos = pos;
    if (dur > 0) {
        lastDur = dur;
        if (track >= 0 && !durations.contains(track))
            durations.insert(track, dur);
    }
    setTextIfChanged(elapsed, hms(pos));
    setTextIfChanged(total, dur > 0 ? hms(dur) : QString());
    if (slider)
        slider->setValue(pos, dur);

    // Whole book: learned track lengths, the average of those for the rest ("≈" until all are known).
    QString f;
    bool bar = false;
    if (trackNo > 0)
        f = trackCount > 0 ? QString("Pista %1 de %2").arg(trackNo).arg(trackCount) : QString("Pista %1").arg(trackNo);
    if (trackCount > 0 && dur > 0 && track >= 0 && track < trackCount) {
        qint64 known = 0;
        int nk = 0;
        for (QMap<int, qint64>::const_iterator it = durations.constBegin(); it != durations.constEnd(); ++it)
            if (it.key() >= 0 && it.key() < trackCount) {
                known += it.value();
                nk++;
            }
        const qint64 avg = nk ? known / nk : dur;
        qint64 done = pos, all = 0;
        for (int i = 0; i < trackCount; i++) {
            const qint64 d = i == track ? dur : durations.value(i, avg);
            all += d;
            if (i < track)
                done += d;
        }
        if (all > 0) {
            const int pct = int(qBound<qint64>(0, done * 100 / all, 100));
            f += QString::fromUtf8(" · %1% del libro · quedan %2%3")
                     .arg(pct).arg(nk >= trackCount ? QString() : QString::fromUtf8("≈ ")).arg(span(all - done));
            if (bookBar)
                bookBar->setValue(done, all);
            bar = true;
        }
    }
    setTextIfChanged(footer, f);
    if (bookBar && bookBar->isHidden() == bar)
        bookBar->setVisible(bar);
    updateSleepLabel();
}

void AioAudio::seekTo(qint64 ms) {
    if (!player)
        return;
    const qint64 dur = player->property("duration").toLongLong();
    if (dur > 0 && ms > dur - 1000)
        ms = dur - 1000;
    if (ms < 0)
        ms = 0;
    pendingSeek = -1; // the user chose a place: no resume over it
    resumeTarget = -1;
    QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, ms));
    lastPos = ms;
    QTimer::singleShot(400, this, SLOT(tick()));
}

void AioAudio::seek(qint64 delta) {
    if (player)
        seekTo(player->property("position").toLongLong() + delta);
}

void AioAudio::rewind() { seek(-30000); }
void AioAudio::ahead() { seek(30000); }
void AioAudio::userSkip() { skipClock.start(); }

// "Regresar a Mis libros": save, then close like Nickel's own back button (queued: we are inside the tap).
void AioAudio::goHome() {
    if (property("aioClosing").toBool())
        return; // a second tap before the queued close ran
    setProperty("aioClosing", true);
    if (player) {
        lastPos = player->property("position").toLongLong();
        lastDur = player->property("duration").toLongLong();
    }
    saveState(false);
    static QPointer<AioCloser> closer;
    if (!closer)
        closer = new AioCloser(QCoreApplication::instance());
    closer->reader = reader;
    QMetaObject::invokeMethod(closer, "close", Qt::QueuedConnection);
}

void AioCloser::close() {
    QWidget *r = reader.data();
    reader = 0;
    if (!r)
        return; // the reader closed some other way before this ran
    if (MP3Reader_saveBookmark)
        MP3Reader_saveBookmark(r);
    if (ReadingController_closeReadingView)
        ReadingController_closeReadingView(); // static: deletes the reader (and its AioAudio), not this object
}

void AioAudio::cycleSpeed() {
    speedIndex = (speedIndex + 1) % 4; // 1x -> 1,2x -> 1,5x -> 0,8x
    if (speed)
        speed->setText(QString::fromLatin1(RATE_NAMES[speedIndex]));
    applyRate();
}

void AioAudio::cycleSleep() {
    sleepMode = (sleepMode + 1) % 5; // off -> 15 -> 30 -> 60 min -> end of track -> off
    sleepTimer->stop();
    pauseOnPlay = false;
    if (sleepMode >= 1 && sleepMode <= 3) {
        sleepTimer->start(SLEEP_MIN[sleepMode] * 60000);
        sleepClock.start();
    }
    updateSleepLabel();
}

void AioAudio::sleepFired() {
    sleepTimer->stop();
    sleepMode = 0;
    pauseOnPlay = false;
    // The pause goes around MP3Reader, so its "Play" setText may never come: show it ourselves first
    // (icon, `playing`, tick timer, saved position), then pause.
    if (!playPauseText(playPause.data(), QString::fromLatin1("Play"))) {
        playing = false;
        timer->stop();
    }
    if (player)
        QMetaObject::invokeMethod(player, "pause");
    updateSleepLabel();
}

void AioAudio::endOfTrackCheck() {
    if (pauseOnPlay)
        sleepFired();
}

void AioAudio::updateSleepLabel() {
    QString t;
    if (sleepMode >= 1 && sleepMode <= 3) {
        qint64 left = qint64(SLEEP_MIN[sleepMode]) * 60000 - (sleepClock.isValid() ? sleepClock.elapsed() : 0);
        t = QString("%1 min").arg((qMax<qint64>(0, left) + 59999) / 60000);
    } else if (sleepMode == 4) {
        t = QString::fromUtf8("Fin de pista");
    }
    setTextIfChanged(sleepText, t);
}

void AioAudio::flashSender() {
    if (QLabel *l = qobject_cast<QLabel *>(sender()))
        flash(l);
}

// Tap feedback: inverted for ~150 ms (icons: white on black; text: black background).
void AioAudio::flash(QLabel *l) {
    QVariant inv = l->property("aioInv");
    if (inv.isValid()) {
        l->setPixmap(qvariant_cast<QPixmap>(inv));
    } else {
        l->setProperty("aioDown", true);
        repolish(l);
    }
    flashed.append(QPointer<QLabel>(l));
    flashTimer->start();
}

// Header volume: the headset's own volume (0..127, sent over AVRCP like Kobo's player), shown as a percentage.
void AioAudio::showVolume() {
    if (volumeLabel)
        volumeLabel->setText(QString::number((btVolumeLoad() * 100 + 63) / 127) + "%");
}

void AioAudio::stepVolume(int delta) {
    btVolume = qBound(0, btVolumeLoad() + delta, 127);
    applyVolumeTo(player.data()); // no headset: the player's own volume, so the buttons still do something
    showVolume();
    if (!onboardReady())
        return;
    QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
    s.setValue("btVolume", btVolume); // one small write per tap, never on a timer
}

void AioAudio::syncVolume() {
    if (volClock.isValid() && volClock.elapsed() < 20000)
        return; // at most every 20 s: libnickel's Bluetooth calls go over D-Bus on the GUI thread
    volClock.start();
    applyVolumeTo(player.data());
}

void AioAudio::volumeDown() { stepVolume(-8); }
void AioAudio::volumeUp() { stepVolume(8); }

void AioAudio::unflash() {
    for (int i = 0; i < flashed.size(); i++) {
        QLabel *l = flashed.at(i).data();
        if (!l)
            continue;
        QVariant icon = l->property("aioIcon");
        if (icon.isValid()) {
            l->setPixmap(qvariant_cast<QPixmap>(icon));
        } else {
            l->setProperty("aioDown", false);
            repolish(l);
        }
    }
    flashed.clear();
}

// -- hooks ----------------------------------------------------------------------------------------------

// The last setMedia no AioAudio took (its reader was still being built, or `aio` belongs to an older reader).
static QPointer<QObject> lastMediaPlayer;
static int lastMediaIndex = -1;
static QByteArray lastMediaData;
static bool lastMediaHasData = false;

static void forgetMedia() {
    lastMediaPlayer = 0;
    lastMediaIndex = -1;
    lastMediaData = QByteArray(); // the whole track: let it go
    lastMediaHasData = false;
}

extern "C" __attribute__((visibility("default"))) void _aio_reader(void *self, QWidget *parent, void *state) {
    MP3Reader_MP3Reader(self, parent, state);
    // MP3Reader passes `this` to Ui_MP3Reader::setupUi as the form QWidget, so the QObject is at offset 0.
    QWidget *reader = reinterpret_cast<QWidget *>(self);
    if (qstrcmp(reader->metaObject()->className(), "MP3Reader") || reader->objectName() != "MP3Reader") {
        nh_log("unexpected reader %s, keeping the stock layout", reader->metaObject()->className());
        forgetMedia();
        return;
    }
    aio = new AioAudio(reader);
    // Stock setPage may already have called setMedia inside the constructor, before AioAudio existed.
    if (lastMediaPlayer && aio->owns(lastMediaPlayer.data()))
        aio->media(lastMediaPlayer.data(), lastMediaIndex, lastMediaData, lastMediaHasData);
    else if (lastMediaPlayer)
        nh_log("AIO Audio: pending track is not this reader's player (MP3Reader+0x40), dropped");
    forgetMedia();
}

extern "C" __attribute__((visibility("default"))) void _aio_setText(QLabel *self, const QString &text) {
    if (!self) {
        QLabel_setText(self, text);
        return;
    }
    if (text == QLatin1String("Pause") && self->objectName() == QLatin1String("playPause")) {
        if (aio)
            aio->syncVolume(); // now playing: make sure the headset is not at volume 0
        else
            applyVolumeTo(0);
    }
    if (aio && aio->playPauseText(self, text))
        return; // play/pause icon
    if (self->property("aioOwned").toBool())
        return; // a stock button we restyled: keep our icon/text
    const QString t = translate(self, text, !aio.isNull());
    if (self->objectName() == QLatin1String("title")) {
        self->setProperty("aioName", t); // read by AioAudio's constructor when setPage ran before it
        if (aio)
            aio->setChapter(t);
    }
    QLabel_setText(self, t);
}

extern "C" __attribute__((visibility("default"))) void _aio_setPixmap(QLabel *self, const QPixmap &pixmap) {
    if (self && self->property("aioCover").toBool())
        return; // our cover, read from the track itself, stays
    QLabel_setPixmap(self, pixmap);
}

// The stock cover lambda moves coverImage to (0,0) at the picture's native size before setPixmap.
extern "C" __attribute__((visibility("default"))) void _aio_setGeometry(QWidget *self, const QRect &rect) {
    if (self && self->property("aioCover").toBool())
        return;
    QWidget_setGeometry(self, rect);
}

extern "C" __attribute__((visibility("default"))) void _aio_setMedia(void *self, int index, const void *content, QIODevice *device) {
    AudioPlayer_setMedia(self, index, content, device);
    QObject *player = reinterpret_cast<QObject *>(self); // AudioPlayer : QMediaPlayer (single inheritance)
    // AUDIO-FIX: stock setPage just called applyVolume(10) (10 % stream gain) and never sets the headset's
    // volume. Full stream gain, and the headset volume is what the volume buttons change (like Kobo's player).
    // Only with the whole Bluetooth API: otherwise the stock volume stays.
    applyVolumeTo(player);
    QBuffer *buffer = qobject_cast<QBuffer *>(device);
    const QByteArray data = buffer ? buffer->data() : QByteArray();
    if (aio && aio->owns(player)) {
        forgetMedia();
        aio->media(player, index, data, buffer != 0);
        return;
    }
    // No AioAudio for this reader yet (setPage inside the MP3Reader constructor): _aio_reader replays it.
    lastMediaPlayer = player;
    lastMediaIndex = index;
    lastMediaData = data;
    lastMediaHasData = buffer != 0;
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

static struct nh_hook AioAudioHook[] = {
    {.sym = "_ZN9MP3ReaderC1EP7QWidgetP11PluginState", .sym_new = "_aio_reader", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_MP3Reader), .desc = "player layout", .optional = true},
    {.sym = "_ZN6QLabel7setTextERK7QString", .sym_new = "_aio_setText", .lib = MP3Z, .out = nh_symoutptr(QLabel_setText), .desc = "Spanish labels", .optional = true},
    {.sym = "_ZN6QLabel9setPixmapERK7QPixmap", .sym_new = "_aio_setPixmap", .lib = MP3Z, .out = nh_symoutptr(QLabel_setPixmap), .desc = "keep cover", .optional = true},
    {.sym = "_ZN7QWidget11setGeometryERK5QRect", .sym_new = "_aio_setGeometry", .lib = MP3Z, .out = nh_symoutptr(QWidget_setGeometry), .desc = "keep cover place", .optional = true},
    {.sym = "_ZN11AudioPlayer8setMediaEiRK13QMediaContentP9QIODevice", .sym_new = "_aio_setMedia", .lib = MP3Z, .out = nh_symoutptr(AudioPlayer_setMedia), .desc = "cover and seeking", .optional = true},
    {0},
};

static struct nh_dlsym AioAudioDlsym[] = {
    {.name = "_ZN10TouchLabel19setDeselectedPixmapERK7QPixmap", .out = nh_symoutptr(TouchLabel_setDeselectedPixmap), .desc = "tap feedback (normal)", .optional = true},
    {.name = "_ZN10TouchLabel17setSelectedPixmapERK7QPixmap", .out = nh_symoutptr(TouchLabel_setSelectedPixmap), .desc = "tap feedback (pressed)", .optional = true},
    {.name = "_ZN17ReadingController16closeReadingViewEv", .out = nh_symoutptr(ReadingController_closeReadingView), .desc = "back to My Books", .optional = true},
    {.name = "_ZN16BluetoothManager14sharedInstanceEv", .out = nh_symoutptr(BluetoothManager_sharedInstance), .desc = "headset volume", .optional = true},
    {.name = "_ZNK16BluetoothManager8a2dpSinkEv", .out = nh_symoutptr(BluetoothManager_a2dpSink), .desc = "headset volume", .optional = true},
    {.name = "_ZNK16BluetoothManager22supportsAbsoluteVolumeERK7QString", .out = nh_symoutptr(BluetoothManager_supportsAbsoluteVolume), .desc = "headset volume", .optional = true},
    {.name = "_ZN16BluetoothManager17setSoftwareVolumeERK7QStringb", .out = nh_symoutptr(BluetoothManager_setSoftwareVolume), .desc = "headset volume", .optional = true},
    {.name = "_ZN16BluetoothManager8setMutedERK7QStringbb", .out = nh_symoutptr(BluetoothManager_setMuted), .desc = "headset volume", .optional = true},
    {.name = "_ZN16BluetoothManager9setVolumeERK7QStringhh", .out = nh_symoutptr(BluetoothManager_setVolume), .desc = "headset volume", .optional = true},
    {0},
};

NickelHook(
    .init  = aio_init,
    .info  = &AioAudioInfo,
    .hook  = AioAudioHook,
    .dlsym = AioAudioDlsym,
)
