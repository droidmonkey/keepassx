/*
*  Copyright (C) 2013 Francois Ferrand
*  Copyright (C) 2017 KeePassXC Team <team@keepassxc.org>
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 2 or (at your option)
*  version 3 of the License.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QInputDialog>
#include <QMessageBox>
#include <QProgressDialog>

#include "Service.h"
#include "Protocol.h"
#include "EntryConfig.h"
#include "AccessControlDialog.h"
#include "HttpSettings.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntrySearcher.h"
#include "core/Global.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/PasswordGenerator.h"
#include "core/Tools.h"
#include "core/Uuid.h"

#include <algorithm>

static const unsigned char KEEPASSHTTP_UUID_DATA[] = {
    0x34, 0x69, 0x7a, 0x40, 0x8a, 0x5b, 0x41, 0xc0,
    0x9f, 0x36, 0x89, 0x7d, 0x62, 0x3e, 0xcb, 0x31
};
static const Uuid KEEPASSHTTP_UUID = Uuid(QByteArray::fromRawData(reinterpret_cast<const char *>(KEEPASSHTTP_UUID_DATA), sizeof(KEEPASSHTTP_UUID_DATA)));
static const char KEEPASSHTTP_NAME[] = "KeePassHttp Settings";
static const char ASSOCIATE_KEY_PREFIX[] = "AES Key: ";
static const char KEEPASSHTTP_GROUP_NAME[] = "KeePassHttp Passwords";   //Group where new KeePassHttp password are stored
static int        KEEPASSHTTP_DEFAULT_ICON = 1;
//private const int DEFAULT_NOTIFICATION_TIME = 5000;

Service::Service(DatabaseTabWidget* parent) :
    KeepassHttpProtocol::Server(parent),
    m_dbTabWidget(parent)
{
    if (HttpSettings::isEnabled())
        start();
}

Entry* Service::getConfigEntry(bool create)
{
    if (DatabaseWidget * dbWidget = m_dbTabWidget->currentDatabaseWidget())
        if (Database * db = dbWidget->database()) {
            Entry* entry = db->resolveEntry(KEEPASSHTTP_UUID);
            if (!entry && create) {
                entry = new Entry();
                entry->setTitle(QLatin1String(KEEPASSHTTP_NAME));
                entry->setUuid(KEEPASSHTTP_UUID);
                entry->setAutoTypeEnabled(Tools::TriState::Disable);
                entry->setGroup(db->rootGroup());
            } else if (entry && entry->group() == db->metadata()->recycleBin()) {
                if (create)
                    entry->setGroup(db->rootGroup());
                else
                    entry = NULL;
            }
            return entry;
        }
    return NULL;
}

bool Service::isDatabaseOpened() const
{
    if (DatabaseWidget* dbWidget = m_dbTabWidget->currentDatabaseWidget())
        switch(dbWidget->currentMode()) {
        case DatabaseWidget::None:
        case DatabaseWidget::LockedMode:
            break;

        case DatabaseWidget::ViewMode:
        case DatabaseWidget::EditMode:
            return true;
        default:
            break;
        }
    return false;
}

bool Service::openDatabase()
{
    if (!HttpSettings::unlockDatabase())
        return false;
    if (DatabaseWidget * dbWidget = m_dbTabWidget->currentDatabaseWidget()) {
        switch(dbWidget->currentMode()) {
        case DatabaseWidget::None:
        case DatabaseWidget::LockedMode:
            break;

        case DatabaseWidget::ViewMode:
        case DatabaseWidget::EditMode:
            return true;
        default:
            break;
        }
    }
    //if (HttpSettings::showNotification()
    //    && !ShowNotification(QString("%0: %1 is requesting access, click to allow or deny")
    //                                 .arg(id).arg(submitHost.isEmpty() ? host : submithost));
    //    return false;
    m_dbTabWidget->activateWindow();
    //Wait a bit for DB to be open... (w/ asynchronous reply?)
    return false;
}

QString Service::getDatabaseRootUuid()
{
    if (DatabaseWidget* dbWidget = m_dbTabWidget->currentDatabaseWidget())
        if (Database* db = dbWidget->database())
            if (Group* rootGroup = db->rootGroup())
                return rootGroup->uuid().toHex();
    return QString();
}

QString Service::getDatabaseRecycleBinUuid()
{
    if (DatabaseWidget* dbWidget = m_dbTabWidget->currentDatabaseWidget())
        if (Database* db = dbWidget->database())
            if (Group* recycleBin = db->metadata()->recycleBin())
                return recycleBin->uuid().toHex();
    return QString();
}

QString Service::getKey(const QString &id)
{
    if (Entry* config = getConfigEntry())
        return config->attributes()->value(QLatin1String(ASSOCIATE_KEY_PREFIX) + id);
    return QString();
}

QString Service::storeKey(const QString &key)
{
    QString id;
    if (Entry* config = getConfigEntry(true)) {

        //ShowNotification("New key association requested")

        do {
            bool ok;
            //Indicate who wants to associate, and request user to enter the 'name' of association key
            id = QInputDialog::getText(0,
                    tr("KeePassXC: New key association request"),
                    tr("You have received an association "
                       "request for the above key.\n"
                       "If you would like to allow it access "
                       "to your KeePassXC database\n"
                       "give it a unique name to identify and accept it."),
                    QLineEdit::Normal, QString(), &ok);
            if (!ok || id.isEmpty())
                return QString();

            //Warn if association key already exists
        } while(config->attributes()->contains(QLatin1String(ASSOCIATE_KEY_PREFIX) + id) &&
                QMessageBox::warning(0, tr("KeePassXC: Overwrite existing key?"),
                                     tr("A shared encryption-key with the name \"%1\" already exists.\nDo you want to overwrite it?").arg(id),
                                     QMessageBox::Yes | QMessageBox::No) == QMessageBox::No);

        config->attributes()->set(QLatin1String(ASSOCIATE_KEY_PREFIX) + id, key, true);
    }
    return id;
}

bool Service::matchUrlScheme(const QString & url)
{
    QString str = url.left(8).toLower();
    return str.startsWith("http://") ||
           str.startsWith("https://") ||
           str.startsWith("ftp://") ||
           str.startsWith("ftps://");
}

bool Service::removeFirstDomain(QString & hostname)
{
    int pos = hostname.indexOf(".");
    if (pos < 0)
        return false;
    hostname = hostname.mid(pos + 1);
    return !hostname.isEmpty();
}

QList<Entry*> Service::searchEntries(Database* db, const QString& hostname)
{
    QList<Entry*> entries;
    if (Group* rootGroup = db->rootGroup()) {
        const auto results = EntrySearcher().search(hostname, rootGroup, Qt::CaseInsensitive);
        for (Entry* entry: results) {
            QString title = entry->title();
            QString url = entry->webUrl();

            //Filter to match hostname in Title and Url fields
            if (   (!title.isEmpty() && hostname.contains(title))
                || (!url.isEmpty() && hostname.contains(url))
                || (matchUrlScheme(title) && hostname.endsWith(QUrl(title).host()))
                || (matchUrlScheme(url) && hostname.endsWith(QUrl(url).host())) )
                entries.append(entry);
        }
    }
    return entries;
}

QList<Entry*> Service::searchEntries(const QString& text)
{
    //Get the list of databases to search
    QList<Database*> databases;
    if (HttpSettings::searchInAllDatabases()) {
        for (int i = 0; i < m_dbTabWidget->count(); i++)
            if (DatabaseWidget* dbWidget = qobject_cast<DatabaseWidget*>(m_dbTabWidget->widget(i)))
                if (Database* db = dbWidget->database())
                    databases << db;
    }
    else if (DatabaseWidget* dbWidget = m_dbTabWidget->currentDatabaseWidget()) {
        if (Database* db = dbWidget->database())
            databases << db;
    }

    //Search entries matching the hostname
    QString hostname = QUrl(text).host();
    QList<Entry*> entries;
    do {
        for (Database* db: asConst(databases)) {
            entries << searchEntries(db, hostname);
        }
    } while(entries.isEmpty() && removeFirstDomain(hostname));

    return entries;
}

Service::Access Service::checkAccess(const Entry *entry, const QString & host, const QString & submitHost, const QString & realm)
{
    EntryConfig config;
    if (!config.load(entry))
        return Unknown;  //not configured
    if ((config.isAllowed(host)) && (submitHost.isEmpty() || config.isAllowed(submitHost)))
        return Allowed;  //allowed
    if ((config.isDenied(host)) || (!submitHost.isEmpty() && config.isDenied(submitHost)))
        return Denied;   //denied
    if (!realm.isEmpty() && config.realm() != realm)
        return Denied;
    return Unknown;      //not configured for this host
}

KeepassHttpProtocol::Entry Service::prepareEntry(const Entry* entry)
{
    KeepassHttpProtocol::Entry res(entry->resolveMultiplePlaceholders(entry->title()),
                                   entry->resolveMultiplePlaceholders(entry->username()),
                                   entry->resolveMultiplePlaceholders(entry->password()),
                                   entry->uuid().toHex());
    if (HttpSettings::supportKphFields()) {
        const EntryAttributes * attr = entry->attributes();
        const auto keys = attr->keys();
        for (const QString& key: keys) {
            if (key.startsWith(QLatin1String("KPH: "))) {
                res.addStringField(key, entry->resolveMultiplePlaceholders(attr->value(key)));
            }
        }
    }
    return res;
}

int Service::sortPriority(const Entry* entry, const QString& host, const QString& submitUrl, const QString& baseSubmitUrl) const
{
    QUrl url(entry->url());
    if (url.scheme().isEmpty())
        url.setScheme("http");
    const QString entryURL = url.toString(QUrl::StripTrailingSlash);
    const QString baseEntryURL = url.toString(QUrl::StripTrailingSlash | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);

    if (submitUrl == entryURL)
        return 100;
    if (submitUrl.startsWith(entryURL) && entryURL != host && baseSubmitUrl != entryURL)
        return 90;
    if (submitUrl.startsWith(baseEntryURL) && entryURL != host && baseSubmitUrl != baseEntryURL)
        return 80;
    if (entryURL == host)
        return 70;
    if (entryURL == baseSubmitUrl)
        return 60;
    if (entryURL.startsWith(submitUrl))
        return 50;
    if (entryURL.startsWith(baseSubmitUrl) && baseSubmitUrl != host)
        return 40;
    if (submitUrl.startsWith(entryURL))
        return 30;
    if (submitUrl.startsWith(baseEntryURL))
        return 20;
    if (entryURL.startsWith(host))
        return 10;
    if (host.startsWith(entryURL))
        return 5;
    return 0;
}

class Service::SortEntries
{
public:
    SortEntries(const QHash<const Entry*, int>& priorities, const QString & field):
        m_priorities(priorities), m_field(field)
    {}

    bool operator()(const Entry* left, const Entry* right) const
    {
        int res = m_priorities.value(left) - m_priorities.value(right);
        if (res == 0)
            return QString::localeAwareCompare(left->attributes()->value(m_field), right->attributes()->value(m_field)) < 0;
        return res < 0;
    }

private:
    const QHash<const Entry*, int>& m_priorities;
    const QString m_field;
};

QList<KeepassHttpProtocol::Entry> Service::findMatchingEntries(const QString& /*id*/, const QString& url, const QString& submitUrl, const QString& realm)
{
    const bool alwaysAllowAccess = HttpSettings::alwaysAllowAccess();
    const QString host = QUrl(url).host();
    const QString submitHost = QUrl(submitUrl).host();

    //Check entries for authorization
    QList<Entry*> pwEntriesToConfirm;
    QList<Entry*> pwEntries;
    const auto entries = searchEntries(url);
    for (Entry* entry: entries) {
        switch(checkAccess(entry, host, submitHost, realm)) {
        case Denied:
            continue;

        case Unknown:
            if (alwaysAllowAccess)
                pwEntries.append(entry);
            else
                pwEntriesToConfirm.append(entry);
            break;

        case Allowed:
            pwEntries.append(entry);
            break;
        }
    }

    //If unsure, ask user for confirmation
    //if (!pwEntriesToConfirm.isEmpty()
    //    && HttpSettings::showNotification()
    //    && !ShowNotification(QString("%0: %1 is requesting access, click to allow or deny")
    //                                 .arg(id).arg(submitHost.isEmpty() ? host : submithost));
    //    pwEntriesToConfirm.clear(); //timeout --> do not request confirmation

    if (!pwEntriesToConfirm.isEmpty()) {

        AccessControlDialog dlg;
        dlg.setUrl(url);
        dlg.setItems(pwEntriesToConfirm);
        //dlg.setRemember();        //TODO: setting!

        int res = dlg.exec();
        if (dlg.remember()) {
            for (Entry* entry: asConst(pwEntriesToConfirm)) {
                EntryConfig config;
                config.load(entry);
                if (res == QDialog::Accepted) {
                    config.allow(host);
                    if (!submitHost.isEmpty() && host != submitHost)
                        config.allow(submitHost);
                } else if (res == QDialog::Rejected) {
                    config.deny(host);
                    if (!submitHost.isEmpty() && host != submitHost)
                        config.deny(submitHost);
                }
                if (!realm.isEmpty())
                    config.setRealm(realm);
                config.save(entry);
            }
        }
        if (res == QDialog::Accepted)
            pwEntries.append(pwEntriesToConfirm);
    }

    //Sort results
    const bool sortSelection = true;
    if (sortSelection) {
        QUrl url(submitUrl);
        if (url.scheme().isEmpty())
            url.setScheme("http");
        const QString submitUrl = url.toString(QUrl::StripTrailingSlash);
        const QString baseSubmitURL = url.toString(QUrl::StripTrailingSlash | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);

        //Cache priorities
        QHash<const Entry*, int> priorities;
        priorities.reserve(pwEntries.size());
        for (const Entry* entry: asConst(pwEntries)) {
            priorities.insert(entry, sortPriority(entry, host, submitUrl, baseSubmitURL));
        }

        //Sort by priorities
        std::sort(pwEntries.begin(), pwEntries.end(), SortEntries(priorities, HttpSettings::sortByTitle() ? "Title" : "UserName"));
    }

    //Fill the list
    QList<KeepassHttpProtocol::Entry> result;
    result.reserve(pwEntries.count());
    for (Entry* entry: asConst(pwEntries)) {
        result << prepareEntry(entry);
    }
    return result;
}

int Service::countMatchingEntries(const QString &, const QString &url, const QString &, const QString &)
{
    return searchEntries(url).count();
}

QList<KeepassHttpProtocol::Entry> Service::searchAllEntries(const QString &)
{
    QList<KeepassHttpProtocol::Entry> result;
    if (DatabaseWidget* dbWidget = m_dbTabWidget->currentDatabaseWidget()) {
        if (Database* db = dbWidget->database()) {
            if (Group* rootGroup = db->rootGroup()) {
                const auto entries = rootGroup->entriesRecursive();
                for (Entry* entry: entries) {
                    if (!entry->url().isEmpty() || QUrl(entry->title()).isValid()) {
                        result << KeepassHttpProtocol::Entry(entry->title(), entry->username(),
                                                             QString(), entry->uuid().toHex());
                    }
                }
            }
        }
    }
    return result;
}

Group * Service::findCreateAddEntryGroup()
{
    if (DatabaseWidget * dbWidget = m_dbTabWidget->currentDatabaseWidget())
        if (Database * db = dbWidget->database())
            if (Group * rootGroup = db->rootGroup()) {
                //TODO: setting to decide where new keys are created
                const QString groupName = QLatin1String(KEEPASSHTTP_GROUP_NAME);

                const auto groups = rootGroup->groupsRecursive(true);
                for (const Group * g: groups) {
                    if (g->name() == groupName) {
                        return db->resolveGroup(g->uuid());
                    }
                }

                Group * group;
                group = new Group();
                group->setUuid(Uuid::random());
                group->setName(groupName);
                group->setIcon(KEEPASSHTTP_DEFAULT_ICON);
                group->setParent(rootGroup);
                return group;
            }
    return NULL;
}

void Service::addEntry(const QString &, const QString &login, const QString &password, const QString &url, const QString &submitUrl, const QString &realm)
{
    if (Group * group = findCreateAddEntryGroup()) {
        Entry * entry = new Entry();
        entry->setUuid(Uuid::random());
        entry->setTitle(QUrl(url).host());
        entry->setUrl(url);
        entry->setIcon(KEEPASSHTTP_DEFAULT_ICON);
        entry->setUsername(login);
        entry->setPassword(password);
        entry->setGroup(group);

        const QString host = QUrl(url).host();
        const QString submitHost = QUrl(submitUrl).host();
        EntryConfig config;
        config.allow(host);
        if (!submitHost.isEmpty())
            config.allow(submitHost);
        if (!realm.isEmpty())
            config.setRealm(realm);
        config.save(entry);
    }
}

void Service::updateEntry(const QString &, const QString &uuid, const QString &login, const QString &password, const QString &url)
{
    if (DatabaseWidget * dbWidget = m_dbTabWidget->currentDatabaseWidget())
        if (Database * db = dbWidget->database())
            if (Entry * entry = db->resolveEntry(Uuid::fromHex(uuid))) {
                QString u = entry->username();
                if (u != login || entry->password() != password) {
                    //ShowNotification(QString("%0: You have an entry change prompt waiting, click to activate").arg(requestId));
                    if (   HttpSettings::alwaysAllowUpdate()
                        || QMessageBox::warning(0, tr("KeePassXC: Update Entry"),
                                                tr("Do you want to update the information in %1 - %2?")
                                                .arg(QUrl(url).host().toHtmlEscaped()).arg(u.toHtmlEscaped()),
                                                QMessageBox::Yes|QMessageBox::No) == QMessageBox::Yes ) {
                        entry->beginUpdate();
                        entry->setUsername(login);
                        entry->setPassword(password);
                        entry->endUpdate();
                    }
                }
            }
}

QString Service::generatePassword()
{
    return HttpSettings::generatePassword();
}

void Service::removeSharedEncryptionKeys()
{
    if (!isDatabaseOpened()) {
        QMessageBox::critical(0, tr("KeePassXC: Database locked!"),
                              tr("The active database is locked!\n"
                                 "Please unlock the selected database or choose another one which is unlocked."),
                              QMessageBox::Ok);
    } else if (Entry* entry = getConfigEntry()) {
        QStringList keysToRemove;
        const auto keys = entry->attributes()->keys();
        for (const QString& key: keys) {
            if (key.startsWith(ASSOCIATE_KEY_PREFIX)) {
                keysToRemove << key;
            }
        }

        if(keysToRemove.count()) {
            entry->beginUpdate();
            for (const QString& key: asConst(keysToRemove)) {
                entry->attributes()->remove(key);
            }
            entry->endUpdate();

            const int count = keysToRemove.count();
            QMessageBox::information(0, tr("KeePassXC: Removed keys from database"),
                                     tr("Successfully removed %1 encryption-%2 from KeePassX/Http Settings.").arg(count).arg(count ? "keys" : "key"),
                                     QMessageBox::Ok);
        } else {
            QMessageBox::information(0, tr("KeePassXC: No keys found"),
                                     tr("No shared encryption-keys found in KeePassHttp Settings."),
                                     QMessageBox::Ok);
        }
    } else {
        QMessageBox::information(0, tr("KeePassXC: Settings not available!"),
                                 tr("The active database does not contain an entry of KeePassHttp Settings."),
                                 QMessageBox::Ok);
    }
}

void Service::removeStoredPermissions()
{
    if (!isDatabaseOpened()) {
        QMessageBox::critical(0, tr("KeePassXC: Database locked!"),
                              tr("The active database is locked!\n"
                                 "Please unlock the selected database or choose another one which is unlocked."),
                              QMessageBox::Ok);
    } else {
        Database * db = m_dbTabWidget->currentDatabaseWidget()->database();
        QList<Entry*> entries = db->rootGroup()->entriesRecursive();

        QProgressDialog progress(tr("Removing stored permissions..."), tr("Abort"), 0, entries.count());
        progress.setWindowModality(Qt::WindowModal);

        uint counter = 0;
        for (Entry* entry: asConst(entries)) {
            if (progress.wasCanceled())
                return;
            if (entry->attributes()->contains(KEEPASSHTTP_NAME)) {
                entry->beginUpdate();
                entry->attributes()->remove(KEEPASSHTTP_NAME);
                entry->endUpdate();
                counter ++;
            }
            progress.setValue(progress.value() + 1);
        }
        progress.reset();

        if (counter > 0) {
            QMessageBox::information(0, tr("KeePassXC: Removed permissions"),
                                     tr("Successfully removed permissions from %1 %2.").arg(counter).arg(counter ? "entries" : "entry"),
                                     QMessageBox::Ok);
        } else {
            QMessageBox::information(0, tr("KeePassXC: No entry with permissions found!"),
                                     tr("The active database does not contain an entry with permissions."),
                                     QMessageBox::Ok);
        }
    }
}
