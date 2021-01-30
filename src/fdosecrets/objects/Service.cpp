/*
 *  Copyright (C) 2018 Aetf <aetf@unlimitedcodeworks.xyz>
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

#include "Service.h"

#include "fdosecrets/FdoSecretsPlugin.h"
#include "fdosecrets/FdoSecretsSettings.h"
#include "fdosecrets/objects/Collection.h"
#include "fdosecrets/objects/Item.h"
#include "fdosecrets/objects/Prompt.h"
#include "fdosecrets/objects/Session.h"

#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"

#include <QDBusConnection>
#include <QDebug>
#include <QSharedPointer>

namespace
{
    constexpr auto DEFAULT_ALIAS = "default";
}

namespace FdoSecrets
{
    QSharedPointer<Service> Service::Create(FdoSecretsPlugin* plugin, QPointer<DatabaseTabWidget> dbTabs, DBusMgr& dbus)
    {
        QSharedPointer<Service> res{new Service(plugin, std::move(dbTabs), dbus)};
        if (!res->initialize()) {
            return {};
        }
        return res;
    }

    Service::Service(FdoSecretsPlugin* plugin,
                     QPointer<DatabaseTabWidget> dbTabs,
                     DBusMgr& dbus) // clazy: exclude=ctor-missing-parent-argument
        : DBusObject(nullptr, dbus)
        , m_plugin(plugin)
        , m_databases(std::move(dbTabs))
        , m_insideEnsureDefaultAlias(false)
    {
        connect(
            m_databases, &DatabaseTabWidget::databaseUnlockDialogFinished, this, &Service::doneUnlockDatabaseInDialog);
    }

    Service::~Service() = default;

    bool Service::initialize()
    {
        if (!dbus().registerObject(this)) {
            return false;
        }

        // Add existing database tabs
        for (int idx = 0; idx != m_databases->count(); ++idx) {
            auto dbWidget = m_databases->databaseWidgetFromIndex(idx);
            onDatabaseTabOpened(dbWidget, false);
        }

        // Connect to new database signal
        // No need to connect to close signal, as collection will remove itself when backend delete/close database tab.
        connect(m_databases.data(), &DatabaseTabWidget::databaseOpened, this, [this](DatabaseWidget* dbWidget) {
            onDatabaseTabOpened(dbWidget, true);
        });

        // make default alias track current activated database
        connect(m_databases.data(), &DatabaseTabWidget::activateDatabaseChanged, this, &Service::ensureDefaultAlias);

        return true;
    }

    void Service::onDatabaseTabOpened(DatabaseWidget* dbWidget, bool emitSignal)
    {
        // The Collection will monitor the database's exposed group.
        // When the Collection finds that no exposed group, it will delete itself.
        // Thus the service also needs to monitor it and recreate the collection if the user changes
        // from no exposed to exposed something.
        connect(dbWidget, &DatabaseWidget::databaseReplaced, this, [this, dbWidget]() {
            monitorDatabaseExposedGroup(dbWidget);
        });
        if (!dbWidget->isLocked()) {
            monitorDatabaseExposedGroup(dbWidget);
        }

        auto coll = Collection::Create(this, dbWidget);
        if (!coll) {
            return;
        }

        m_collections << coll;
        m_dbToCollection[dbWidget] = coll;

        // keep record of the collection existence
        connect(coll, &Collection::collectionAboutToDelete, this, [this, coll]() {
            m_collections.removeAll(coll);
            m_dbToCollection.remove(coll->backend());
        });

        // keep record of alias
        connect(coll, &Collection::aliasAboutToAdd, this, &Service::onCollectionAliasAboutToAdd);
        connect(coll, &Collection::aliasAdded, this, &Service::onCollectionAliasAdded);
        connect(coll, &Collection::aliasRemoved, this, &Service::onCollectionAliasRemoved);

        // Forward delete signal, we have to rely on filepath to identify the database being closed,
        // but we can not access m_backend safely because during the databaseClosed signal,
        // m_backend may already be reset to nullptr
        // We want to remove the collection object from dbus as early as possible, to avoid
        // race conditions when deleteLater was called on the m_backend, but not delivered yet,
        // and new method calls from dbus occurred. Therefore we can't rely on the destroyed
        // signal on m_backend.
        // bind to coll lifespan
        connect(m_databases.data(), &DatabaseTabWidget::databaseClosed, coll, [coll](const QString& filePath) {
            if (filePath == coll->backendFilePath()) {
                coll->doDelete();
            }
        });

        // actual load, must after updates to m_collections, because the reload may trigger
        // another onDatabaseTabOpen, and m_collections will be used to prevent recursion.
        if (!coll->reloadBackend()) {
            // error in dbus
            return;
        }
        if (!coll->backend()) {
            // no exposed group on this db
            return;
        }

        ensureDefaultAlias();

        // only start relay signals when the collection is fully setup
        connect(coll, &Collection::collectionChanged, this, [this, coll]() { emit collectionChanged(coll); });
        connect(coll, &Collection::collectionAboutToDelete, this, [this, coll]() { emit collectionDeleted(coll); });
        if (emitSignal) {
            emit collectionCreated(coll);
        }
    }

    void Service::monitorDatabaseExposedGroup(DatabaseWidget* dbWidget)
    {
        Q_ASSERT(dbWidget);
        connect(
            dbWidget->database()->metadata()->customData(), &CustomData::customDataModified, this, [this, dbWidget]() {
                if (!FdoSecrets::settings()->exposedGroup(dbWidget->database()).isNull() && !findCollection(dbWidget)) {
                    onDatabaseTabOpened(dbWidget, true);
                }
            });
    }

    void Service::ensureDefaultAlias()
    {
        if (m_insideEnsureDefaultAlias) {
            return;
        }

        m_insideEnsureDefaultAlias = true;

        auto coll = findCollection(m_databases->currentDatabaseWidget());
        if (coll) {
            // adding alias will automatically remove the association with previous collection.
            coll->addAlias(DEFAULT_ALIAS).okOrDie();
        }

        m_insideEnsureDefaultAlias = false;
    }

    DBusResult Service::collections(QList<Collection*>& collections) const
    {
        collections = m_collections;
        return {};
    }

    DBusResult Service::openSession(const QString& algorithm, const QVariant& input, QVariant& output, Session*& result)
    {
        const auto& client = dbus().callingClient();

        // negotiate cipher
        bool incomplete = false;
        auto ciphers = client->negotiateCipher(algorithm, input, output, incomplete);
        if (incomplete) {
            result = nullptr;
            return {};
        }
        if (!ciphers) {
            return QDBusError::NotSupported;
        }

        // create session using the negotiated cipher
        result = Session::Create(std::move(ciphers), client->name(), this);
        if (!result) {
            return QDBusError::InternalError;
        }

        // remove session when the client disconnects
        connect(&dbus(), &DBusMgr::clientDisconnected, result, [result, client](const DBusClientPtr& toRemove) {
            if (toRemove == client) {
                result->close().okOrDie();
            }
        });

        // keep a list of sessions
        m_sessions.append(result);
        connect(result, &Session::aboutToClose, this, [this, result]() { m_sessions.removeAll(result); });

        return {};
    }

    DBusResult Service::createCollection(const QVariantMap& properties,
                                         const QString& alias,
                                         Collection*& collection,
                                         PromptBase*& prompt)
    {
        prompt = nullptr;

        // return existing collection if alias is non-empty and exists.
        collection = findCollection(alias);
        if (!collection) {
            prompt = PromptBase::Create<CreateCollectionPrompt>(this, properties, alias);
            if (!prompt) {
                return QDBusError::InternalError;
            }
        }
        return {};
    }

    DBusResult
    Service::searchItems(const StringStringMap& attributes, QList<Item*>& unlocked, QList<Item*>& locked) const
    {
        QList<Collection*> colls;
        auto ret = collections(colls);
        if (ret.err()) {
            return ret;
        }

        for (const auto& coll : asConst(colls)) {
            QList<Item*> items;
            ret = coll->searchItems(attributes, items);
            if (ret.err()) {
                return ret;
            }
            // item locked state already covers its collection's locked state
            for (const auto& item : asConst(items)) {
                bool l;
                ret = item->locked(l);
                if (ret.err()) {
                    return ret;
                }
                if (l) {
                    locked.append(item);
                } else {
                    unlocked.append(item);
                }
            }
        }
        return {};
    }

    DBusResult Service::unlock(const QList<DBusObject*>& objects, QList<DBusObject*>& unlocked, PromptBase*& prompt)
    {
        QSet<Collection*> collectionsToUnlock;
        QSet<Item*> itemsToUnlock;
        collectionsToUnlock.reserve(objects.size());
        itemsToUnlock.reserve(objects.size());

        for (const auto& obj : asConst(objects)) {
            // the object is either an item or an collection
            auto item = qobject_cast<Item*>(obj);
            auto coll = item ? item->collection() : qobject_cast<Collection*>(obj);
            // either way there should be a collection
            if (!coll) {
                continue;
            }

            bool collLocked{false}, itemLocked{false};
            // if the collection needs unlock
            auto ret = coll->locked(collLocked);
            if (ret.err()) {
                return ret;
            }
            if (collLocked) {
                collectionsToUnlock << coll;
            }

            if (item) {
                // item may also need unlock
                ret = item->locked(itemLocked);
                if (ret.err()) {
                    return ret;
                }
                if (itemLocked) {
                    itemsToUnlock << item;
                }
            }

            // both collection and item are not locked
            if (!collLocked && !itemLocked) {
                unlocked << obj;
            }
        }

        if (!collectionsToUnlock.isEmpty() || !itemsToUnlock.isEmpty()) {
            prompt = PromptBase::Create<UnlockPrompt>(this, collectionsToUnlock, itemsToUnlock);
            if (!prompt) {
                return QDBusError::InternalError;
            }
        }
        return {};
    }

    DBusResult Service::lock(const QList<DBusObject*>& objects, QList<DBusObject*>& locked, PromptBase*& prompt)
    {
        QSet<Collection*> needLock;
        needLock.reserve(objects.size());
        for (const auto& obj : asConst(objects)) {
            auto coll = qobject_cast<Collection*>(obj);
            if (coll) {
                needLock << coll;
            } else {
                auto item = qobject_cast<Item*>(obj);
                if (!item) {
                    continue;
                }
                // we lock the whole collection for item
                needLock << item->collection();
            }
        }

        // return anything already locked
        QList<Collection*> toLock;
        for (const auto& coll : asConst(needLock)) {
            bool l;
            auto ret = coll->locked(l);
            if (ret.err()) {
                return ret;
            }
            if (l) {
                locked << coll;
            } else {
                toLock << coll;
            }
        }
        if (!toLock.isEmpty()) {
            prompt = PromptBase::Create<LockCollectionsPrompt>(this, toLock);
            if (!prompt) {
                return QDBusError::InternalError;
            }
        }
        return {};
    }

    DBusResult Service::getSecrets(const QList<Item*>& items, Session* session, ItemSecretMap& secrets) const
    {
        if (!session) {
            return DBusResult(DBUS_ERROR_SECRET_NO_SESSION);
        }

        for (const auto& item : asConst(items)) {
            auto ret = item->getSecretNoNotification(session, secrets[item]);
            if (ret.err()) {
                return ret;
            }
        }
        plugin()->emitRequestShowNotification(
            tr(R"(%n Entry(s) was used by %1)", "%1 is the name of an application", secrets.size())
                .arg(dbus().callingClient()->name()));
        return {};
    }

    DBusResult Service::readAlias(const QString& name, Collection*& collection) const
    {
        collection = findCollection(name);
        return {};
    }

    DBusResult Service::setAlias(const QString& name, Collection* collection)
    {
        if (!collection) {
            // remove alias name from its collection
            collection = findCollection(name);
            if (!collection) {
                return DBusResult(DBUS_ERROR_SECRET_NO_SUCH_OBJECT);
            }
            return collection->removeAlias(name);
        }
        return collection->addAlias(name);
    }

    Collection* Service::findCollection(const QString& alias) const
    {
        if (alias.isEmpty()) {
            return nullptr;
        }

        auto it = m_aliases.find(alias);
        if (it != m_aliases.end()) {
            return it.value();
        }
        return nullptr;
    }

    void Service::onCollectionAliasAboutToAdd(const QString& alias)
    {
        auto coll = qobject_cast<Collection*>(sender());

        auto it = m_aliases.constFind(alias);
        if (it != m_aliases.constEnd() && it.value() != coll) {
            // another collection holds the alias
            // remove it first
            it.value()->removeAlias(alias).okOrDie();

            // onCollectionAliasRemoved called through signal
            // `it` becomes invalidated now
        }
    }

    void Service::onCollectionAliasAdded(const QString& alias)
    {
        auto coll = qobject_cast<Collection*>(sender());
        m_aliases[alias] = coll;
    }

    void Service::onCollectionAliasRemoved(const QString& alias)
    {
        m_aliases.remove(alias);
        ensureDefaultAlias();
    }

    Collection* Service::findCollection(const DatabaseWidget* db) const
    {
        return m_dbToCollection.value(db, nullptr);
    }

    const QList<Session*> Service::sessions() const
    {
        return m_sessions;
    }

    bool Service::doCloseDatabase(DatabaseWidget* dbWidget)
    {
        return m_databases->closeDatabaseTab(dbWidget);
    }

    Collection* Service::doNewDatabase()
    {
        auto dbWidget = m_databases->newDatabase();
        if (!dbWidget) {
            return nullptr;
        }

        // database created through dbus will be exposed to dbus by default
        auto db = dbWidget->database();
        FdoSecrets::settings()->setExposedGroup(db, db->rootGroup()->uuid());

        auto collection = findCollection(dbWidget);

        Q_ASSERT(collection);

        return collection;
    }

    void Service::doSwitchToDatabaseSettings(DatabaseWidget* dbWidget)
    {
        if (dbWidget->isLocked()) {
            return;
        }
        // switch selected to current
        m_databases->setCurrentWidget(dbWidget);
        m_databases->showDatabaseSettings();

        // open settings (switch from app settings to m_dbTabs)
        m_plugin->emitRequestSwitchToDatabases();
    }

    void Service::doUnlockDatabaseInDialog(DatabaseWidget* dbWidget)
    {
        m_databases->unlockDatabaseInDialog(dbWidget, DatabaseOpenDialog::Intent::None);
    }

} // namespace FdoSecrets
