#include "tools.h"

#include "board/abstractboard.h"
#include "cache.h"
#include "controller/ban.h"
#include "controller/board.h"
#include "controller/error.h"
#include "controller/notfound.h"
#include "settingslocker.h"
#include "translator.h"

#include <BCoreApplication>
#include <BDirTools>
#include <BLogger>

#include <QByteArray>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLocale>
#include <QMap>
#include <QMutex>
#include <QRegExp>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QVariant>

#include <cppcms/http_cookie.h>
#include <cppcms/http_file.h>
#include <cppcms/http_request.h>
#include <cppcms/json.h>

#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>

#include <cmath>
#include <istream>
#include <list>
#include <locale>
#include <ostream>
#include <streambuf>
#include <string>

namespace Tools
{

struct IpRange
{
    unsigned int start;
    unsigned int end;
public:
    bool operator <(const IpRange &other) const
    {
        return start < other.start;
    }
};

struct membuf : std::streambuf
{
public:
    explicit membuf(char *begin, char *end)
    {
        this->setg(begin, begin, end);
    }
};

static QMutex cityNameMutex(QMutex::Recursive);
static QMutex countryCodeMutex(QMutex::Recursive);
static QMutex countryNameMutex(QMutex::Recursive);
static QMutex storagePathMutex(QMutex::Recursive);
static QMutex timezoneMutex(QMutex::Recursive);

static unsigned int ipNum(const QString &ip, bool *ok = 0)
{
    QStringList sl = ip.split('.');
    if (sl.size() != 4)
        return bRet(ok, false, 0);
    bool b = false;
    unsigned int n = sl.last().toUInt(&b);
    if (!b)
        return bRet(ok, false, 0);
    n += 256 * sl.at(2).toUInt(&b);
    if (!ok)
        return bRet(ok, false, 0);
    n += 256 * 256 * sl.at(1).toUInt(&b);
    if (!ok)
        return bRet(ok, false, 0);
    n += 256 * 256 * 256 * sl.first().toUInt(&b);
    if (!ok || !n)
        return bRet(ok, false, 0);
    return bRet(ok, true, n);
}

static QTime time(int msecs)
{
    int h = msecs / BeQt::Hour;
    msecs %= BeQt::Hour;
    int m = msecs / BeQt::Minute;
    msecs %= BeQt::Minute;
    int s = msecs / BeQt::Second;
    return QTime(h, m, s, msecs % BeQt::Second);
}

bool captchaEnabled(const QString &boardName)
{
    SettingsLocker s;
    return s->value("Board/captcha_enabled", true).toBool()
            && (boardName.isEmpty() || s->value("Board/" + boardName + "/captcha_enabled", true).toBool());
}

QString cityName(const QString &ip)
{
    typedef QMap<IpRange, QString> NameMap;
    QMutexLocker locker(&cityNameMutex);
    init_once(NameMap, names, NameMap()) {
        QString fn = BDirTools::findResource("res/ip_city_name_map.txt");
        QStringList sl = BDirTools::readTextFile(fn, "UTF-8").split(QRegExp("\\r?\\n+"), QString::SkipEmptyParts);
        foreach (const QString &s, sl) {
            QStringList sll = s.split(' ');
            if (sll.size() < 3)
                continue;
            IpRange r;
            bool ok = false;
            r.start = sll.first().toUInt(&ok);
            if (!ok)
                continue;
            r.end = sll.at(1).toUInt(&ok);
            if (!ok)
                continue;
            QString n = QStringList(sll.mid(2)).join(" ");
            if (n.length() < sll.size() - 2)
                continue;
            names.insert(r, n);
        }
    }
    unsigned int n = ipNum(ip);
    if (!n)
        return "";
    foreach (const IpRange &r, names.keys()) {
        if (n >= r.start && n <= r.end)
            return names.value(r);
    }
    return "";
}

QString cityName(const cppcms::http::request &req)
{
    return cityName(fromStd(const_cast<cppcms::http::request *>(&req)->remote_addr()));
}

QString countryCode(const QString &ip)
{
    typedef QMap<IpRange, QString> CodeMap;
    QMutexLocker locker(&countryCodeMutex);
    init_once(CodeMap, codes, CodeMap()) {
        QString fn = BDirTools::findResource("res/ip_country_code_map.txt");
        QStringList sl = BDirTools::readTextFile(fn, "UTF-8").split(QRegExp("\\r?\\n+"), QString::SkipEmptyParts);
        foreach (const QString &s, sl) {
            QStringList sll = s.split(' ');
            if (sll.size() != 3)
                continue;
            IpRange r;
            bool ok = false;
            r.start = sll.first().toUInt(&ok);
            if (!ok)
                continue;
            r.end = sll.at(1).toUInt(&ok);
            if (!ok)
                continue;
            QString c = sll.last();
            if (c.length() != 2)
                continue;
            codes.insert(r, c);
        }
    }
    unsigned int n = ipNum(ip);
    if (!n)
        return "";
    foreach (const IpRange &r, codes.keys()) {
        if (n >= r.start && n <= r.end)
            return codes.value(r);
    }
    return "";
}

QString countryCode(const cppcms::http::request &req)
{
    return countryCode(fromStd(const_cast<cppcms::http::request *>(&req)->remote_addr()));
}

QString countryName(const QString &countryCode)
{
    typedef QMap<QString, QString> NameMap;
    QMutexLocker locker(&countryNameMutex);
    init_once(NameMap, names, NameMap()) {
        QString fn = BDirTools::findResource("res/country_code_name_map.txt");
        QStringList sl = BDirTools::readTextFile(fn, "UTF-8").split(QRegExp("\\r?\\n+"), QString::SkipEmptyParts);
        foreach (const QString &s, sl) {
            QStringList sll = s.split(' ');
            if (sll.size() != 2)
                continue;
            if (sll.first().length() != 2 || sll.last().isEmpty())
                continue;
            names.insert(sll.first(), sll.last());
        }
    }
    if (countryCode.length() != 2)
        return "";
    return names.value(countryCode);
}

QDateTime dateTime(const QDateTime &dt, const cppcms::http::request &req)
{
    const cppcms::http::cookie &cookie = const_cast<cppcms::http::request *>(&req)->cookie_by_name("time");
    QString s = fromStd(cookie.value());
    if (s.isEmpty() || s.compare("local", Qt::CaseInsensitive))
        return localDateTime(dt);
    return localDateTime(dt, timeZoneMinutesOffset(req));
}

void deleteFiles(const QString &boardName, const QStringList &fileNames)
{
    if (boardName.isEmpty())
        return;
    QString path = storagePath() + "/img/" + boardName;
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isDir())
        return;
    foreach (const QString &fn, fileNames) {
        QFile::remove(path + "/" + fn);
        QFileInfo fii(fn);
        QString suff = !fii.suffix().compare("gif", Qt::CaseInsensitive) ? "png" : fii.suffix();
        QFile::remove(path + "/" + fii.baseName() + "s." + suff);
    }
}

QLocale fromStd(const std::locale &l)
{
    return QLocale(fromStd(l.name()).split('.').first());
}

QString fromStd(const std::string &s)
{
    return QString::fromLocal8Bit(s.data());
}

QStringList fromStd(const std::list<std::string> &sl)
{
    QStringList list;
    foreach (const std::string &s, sl)
        list << fromStd(s);
    return list;
}

QString hashPassString(const cppcms::http::request &req)
{
    return Tools::fromStd(const_cast<cppcms::http::request *>(&req)->cookie_by_name("hashpass").value());
}

bool isCaptchaValid(const QString &captcha)
{
    if (captcha.isEmpty())
        return false;
    try {
        curlpp::Cleanup curlppCleanup;
        Q_UNUSED(curlppCleanup)
        QString url = "https://www.google.com/recaptcha/api/siteverify?secret=%1&response=%2";
        url = url.arg(SettingsLocker()->value("Site/captcha_private_key").toString()).arg(captcha);
        curlpp::Easy request;
        request.setOpt(curlpp::options::Url(Tools::toStd(url)));
        std::ostringstream os;
        os << request;
        QString result = Tools::fromStd(os.str());
        result.remove(QRegExp(".*\"success\":\\s*\"?"));
        result.remove(QRegExp("\"?\\,?\\s+.+"));
        return !result.compare("true", Qt::CaseInsensitive);
    } catch (curlpp::RuntimeError &e) {
        qDebug() << e.what();
        return false;
    } catch(curlpp::LogicError &e) {
        qDebug() << e.what();
        return false;
    }
}

QDateTime localDateTime(const QDateTime &dt, int offsetMinutes)
{
    static const int MaxMsecs = 24 * BeQt::Hour;
    if (offsetMinutes < -720 || offsetMinutes > 840)
        return dt.toLocalTime();
    QDateTime ndt = dt.toUTC();
    QTime t = ndt.time();
    int msecs = t.hour() * BeQt::Hour + t.minute() * BeQt::Minute + t.second() * BeQt::Second * t.msec();
    int msecsOffset = offsetMinutes * BeQt::Minute;
    msecs += msecsOffset;
    if (msecs < 0) {
        ndt.setDate(ndt.date().addDays(-1));
        ndt.setTime(time(msecs + MaxMsecs));
    } else if (msecs >= MaxMsecs) {
        ndt.setDate(ndt.date().addDays(1));
        ndt.setTime(time(MaxMsecs - msecs));
    } else {
        ndt.setTime(time(msecs));
    }
    return ndt;
}

QLocale locale(const cppcms::http::request &req, const QLocale &defaultLocale)
{
    cppcms::http::cookie localeCookie = const_cast<cppcms::http::request *>(&req)->cookie_by_name("locale");
    QLocale l(fromStd(localeCookie.value()));
    if (QLocale::c() == l)
        l = QLocale(countryCode(req));
    return (QLocale::c() == l) ? defaultLocale : l;
}

void log(const cppcms::application &app, const QString &what)
{
    log(const_cast<cppcms::application *>(&app)->request(), what);
}

void log(const cppcms::http::request &req, const QString &what)
{
    bLog("[" + userIp(req) + "] " + what);
}

FileList postFiles(const cppcms::http::request &request)
{
    FileList list;
    cppcms::http::request::files_type files = const_cast<cppcms::http::request *>(&request)->files();
    foreach (int i, bRangeD(0, files.size() - 1)) {
        cppcms::http::file *f = files.at(i).get();
        if (!f)
            continue;
        File file;
        std::istream &in = f->data();
        char *buff = new char[f->size()];
        in.read(buff, f->size());
        file.data = QByteArray::fromRawData(buff, f->size());
        file.fileName = QFileInfo(fromStd(f->filename())).fileName();
        file.formFieldName = fromStd(f->name());
        file.mimeType = fromStd(f->mime());
        list << file;
    }
    return list;
}

bool postingEnabled(const QString &boardName)
{
    if (boardName.isEmpty() || !AbstractBoard::boardNames().contains(boardName))
        return false;
    SettingsLocker s;
    return s->value("Board/posting_enabled", true).toBool()
            && s->value("Board/" + boardName + "/posting_enabled", true).toBool();
}

PostParameters postParameters(const cppcms::http::request &request)
{
    PostParameters m;
    cppcms::http::request::form_type data = const_cast<cppcms::http::request *>(&request)->post();
    for (cppcms::http::request::form_type::iterator i = data.begin(); i != data.end(); ++i)
        m.insert(fromStd(i->first), fromStd(i->second));
    return m;
}

cppcms::json::value readJsonValue(const QString &fileName, bool *ok)
{
    bool b = false;
    QByteArray jsonBa = BDirTools::readFile(fileName, -1, &b);
    if (!b)
        return cppcms::json::value();
    char *jsonData = jsonBa.data();
    membuf jsonBuff(jsonData, jsonData + jsonBa.size());
    std::istream jsonIn(&jsonBuff);
    cppcms::json::value json;
    if (json.load(jsonIn, true))
        return bRet(ok, true, json);
    else
        return bRet(ok, false, cppcms::json::value());
}

QStringList rules(const QString &prefix, const QLocale &l)
{
    QStringList *sl = Cache::rules(l, prefix);
    if (!sl) {
        QString path = BDirTools::findResource(prefix, BDirTools::UserOnly);
        if (path.isEmpty())
            return QStringList();
        QString fn = BDirTools::localeBasedFileName(path + "/rules.txt", l);
        if (fn.isEmpty())
            return QStringList();
        sl = new QStringList(BDirTools::readTextFile(fn, "UTF-8").split(QRegExp("\\r?\\n+"), QString::SkipEmptyParts));
        if (!Cache::cacheRules(prefix, l, sl)) {
            QStringList sll = *sl;
            delete sl;
            return sll;
        }
    }
    return *sl;
}

QString saveFile(const File &f, const QString &boardName, bool *ok)
{
    QString storagePath = Tools::storagePath();
    if (boardName.isEmpty() || storagePath.isEmpty())
        return bRet(ok, false, QString());
    QString path = storagePath + "/img/" + boardName;
    if (!BDirTools::mkpath(path))
        return bRet(ok, false, QString());
    QString dt = QString::number(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());
    QString suffix = QFileInfo(f.fileName).suffix();
    QImage img;
    QByteArray data = f.data;
    QBuffer buff(&data);
    buff.open(QIODevice::ReadOnly);
    if (!img.load(&buff, suffix.toLower().toLatin1().data()))
        return bRet(ok, false, QString());
    if (img.height() > 200 || img.width() > 200)
        img = img.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QString sfn = path + "/" + dt + "." + suffix;
    if (!BDirTools::writeFile(sfn, f.data))
        return bRet(ok, false, QString());
    if (!suffix.compare("gif", Qt::CaseInsensitive))
        suffix = "png";
    if (!img.save(path + "/" + dt + "s." + suffix, suffix.toLower().toLatin1().data()))
        return bRet(ok, false, QString());
    return bRet(ok, true, QFileInfo(sfn).fileName());
}

QString storagePath()
{
    storagePathMutex.lock();
    init_once(QString, path, QString())
        path = BDirTools::findResource("storage", BDirTools::UserOnly);
    storagePathMutex.unlock();
    return path;
}

QStringList supportedCodeLanguages()
{
    QString srchighlightPath = BDirTools::findResource("srchilite");
    if (srchighlightPath.isEmpty())
        return QStringList();
    QStringList sl = QDir(srchighlightPath).entryList(QStringList() << "*.lang", QDir::Files);
    foreach (int i, bRangeD(0, sl.size() - 1))
        sl[i].remove(".lang");
    int ind = sl.indexOf("cpp");
    if (ind >= 0)
        sl.insert(ind + 1, "c++");
    return sl;
}

int timeZoneMinutesOffset(const cppcms::http::request &req)
{
    typedef QMap<QString, int> TimezoneMap;
    QMutexLocker locker(&timezoneMutex);
    init_once(TimezoneMap, timezones, TimezoneMap()) {
        QString fn = BDirTools::findResource("res/city_name_timezone_map.txt");
        QStringList sl = BDirTools::readTextFile(fn, "UTF-8").split(QRegExp("\\r?\\n+"), QString::SkipEmptyParts);
        foreach (const QString &s, sl) {
            QStringList sll = s.split(' ');
            if (sll.size() < 2)
                continue;
            if (sll.last().isEmpty())
                continue;
            bool ok = false;
            int offset = sll.last().toInt(&ok);
            if (!ok || offset < -720 || offset > 840)
                continue;
            timezones.insert(QStringList(sll.mid(0, sll.size() - 1)).join(" "), offset);
        }
    }
    return timezones.value(cityName(req), -1000);
}

QByteArray toHashpass(const QString &s, bool *ok)
{
    if (s.length() != 44)
        return bRet(ok, false, QByteArray());
    QStringList sl = s.split('-');
    if (sl.size() != 5)
        return bRet(ok, false, QByteArray());
    QByteArray ba;
    foreach (const QString &ss, sl) {
        if (ss.length() != 8 || !QRegExp("([0-9a-fA-F]){8}").exactMatch(ss))
            return bRet(ok, false, QByteArray());
        char c[4];
        foreach (int i, bRangeD(0, 3)) {
            bool b = false;
            c[i] = ss.mid(i * 2, 2).toUShort(&b, 16);
            if (!b)
                return bRet(ok, false, QByteArray());
        }
        ba.append(c, 4);
    }
    return bRet(ok, true, ba);
}

Post toPost(const PostParameters &params, const FileList &files)
{
    Post p;
    p.captcha = params.value("g-recaptcha-response");
    p.email = params.value("email");
    p.files = files;
    p.name = params.value("name");
    QString pwd = params.value("password");
    if (pwd.isEmpty())
        pwd = SettingsLocker()->value("Board/default_post_password").toString();
    p.password = QCryptographicHash::hash(pwd.toLocal8Bit(), QCryptographicHash::Sha1);;
    p.subject = params.value("subject");
    p.text = params.value("text");
    return p;
}

Post toPost(const cppcms::http::request &req)
{
    return toPost(postParameters(req), postFiles(req));
}

std::locale toStd(const QLocale &l)
{
    return std::locale(toStd(l.name()).data());
}

std::string toStd(const QString &s)
{
    return std::string(s.toLocal8Bit().data());
}

std::list<std::string> toStd(const QStringList &sl)
{
    std::list<std::string> list;
    foreach (const QString &s, sl)
        list.push_back(toStd(s));
    return list;
}

QString toString(const QByteArray &hp, bool *ok)
{
    if (hp.size() != 20)
        return bRet(ok, false, QString());
    QString s;
    foreach (int i, bRangeD(0, hp.size() - 1)) {
        QString c = QString::number(uchar(hp.at(i)), 16);
        if (c.length() < 2)
            c.prepend("0");
        s += c;
        if ((i != hp.size() - 1) && !((i + 1) % 4))
            s += "-";
    }
    bool b = false;
    toHashpass(s, &b);
    if (!b)
        return bRet(ok, false, QString());
    return bRet(ok, true, s);
}

QString userIp(const cppcms::http::request &req)
{
    if (SettingsLocker()->value("System/use_x_real_ip", false).toBool())
        return fromStd(const_cast<cppcms::http::request *>(&req)->getenv("HTTP_X_REAL_IP"));
    else
        return fromStd(const_cast<cppcms::http::request *>(&req)->remote_addr());
}

}
