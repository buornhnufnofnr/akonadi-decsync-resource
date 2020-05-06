/*
 * Copyright (C) 2020 by Timo Wilken <timo.21.wilken@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "decsyncresource.h"

#include "../build/src/settings.h"
#include "../build/src/settingsadaptor.h"
#include "../build/src/debug.h"

#include <QDBusConnection>
#include <QUrl>
#include <QFileDialog>
#include <QHostInfo>

#include <KLocalizedString>
#include <ItemFetchJob>

#include <libdecsync.h>

DecSyncResource::DecSyncResource(const QString &id)
    : ResourceBase(id)
{
    new SettingsAdaptor(Settings::self());
    QDBusConnection::sessionBus().registerObject(
        QStringLiteral("/Settings"),
        Settings::self(),
        QDBusConnection::ExportAdaptors);

    setNeedsNetwork(false);

    const int versionStatus = decsync_check_decsync_info(
        qUtf8Printable(Settings::self()->decSyncDirectory()));
    setOnline(!versionStatus);
    if (versionStatus) {
        const char* errorMessage =
            versionStatus == 1 ? "libdecsync: %s: found invalid .decsync-version" :
            versionStatus == 2 ? "libdecsync: %s: unsupported version" :
            "libdecsync: %s: unknown error";
        Q_EMIT status(Akonadi::AgentBase::Status::Broken, QString::fromUtf8(errorMessage));
        qCCritical(log_decsyncresource, "%s", errorMessage);
        setTemporaryOffline(60);
    }

    decsync_get_app_id("akonadi", this->appId, APPID_LENGTH);
    qCDebug(log_decsyncresource, "resource started with app ID %s", this->appId);
}

/**
 * This method is usually called when a new resource is being added to the
 * Akonadi setup. You can do any kind of user interaction here, e.g. showing
 * dialogs.
 *
 * The given window ID is usually useful to get the correct "on top of parent"
 * behavior if the running window manager applies any kind of focus stealing
 * prevention technique.
 *
 * If the configuration dialog has been accepted by the user by clicking Ok, the
 * signal configurationDialogAccepted() has to be emitted, otherwise, if the
 * user canceled the dialog, configurationDialogRejected() has to be emitted.
 */
void DecSyncResource::configure(WId windowId)
{
    Q_UNUSED(windowId);

    const QString oldPath = Settings::self()->decSyncDirectory();
    const QString newPath = QFileDialog::getExistingDirectory(
        nullptr,
        i18nc("@title:window", "Select DecSync folder"),
        QUrl::fromLocalFile(oldPath.isEmpty() ? QDir::homePath() : oldPath).path());

    if (newPath.isEmpty() || oldPath == newPath ||
        decsync_check_decsync_info(qUtf8Printable(newPath)) != 0) {
        configurationDialogRejected();
        return;
    }

    Settings::self()->setDecSyncDirectory(newPath);
    Settings::self()->save();
    synchronize();
    configurationDialogAccepted();
}

DecSyncResource::~DecSyncResource() {}

/**
 * Any cleanup you need to do while there is still an active event loop. The
 * resource will terminate after this method returns.
 */
void DecSyncResource::aboutToQuit() {}

/**
 * Gets a list of MIME types that a collection of the given DecSync type should
 * support. If the collection type is unknown, returns an empty list.
 */
const QStringList appropriateMimetypes(const char* collectionType)
{
    if (0 == strcmp("calendars", collectionType)) {
        return { QStringLiteral("application/x-vnd.akonadi.calendar.event"),
                 QStringLiteral("text/calendar") };
    } else if (0 == strcmp("contacts", collectionType)) {
        return { QStringLiteral("text/directory") };
    } else {
        return {};
    }
}

void DecSyncResource::retrieveCollections()
{
    Akonadi::Collection::List collections;

    if (Settings::self()->decSyncDirectory().isEmpty()) {
        collectionsRetrieved(collections);
        return;
    }

    for (QList<const char*>::const_iterator type = COLLECTION_TYPES.constBegin();
         type != COLLECTION_TYPES.constEnd(); ++type) {
        const QString qTypeName = QString::fromUtf8(*type);

        Akonadi::Collection parentColl;
        parentColl.setParentCollection(Akonadi::Collection::root());
        parentColl.setRemoteId(qTypeName + QPATHSEP);
        // Allow subcollections only.
        parentColl.setContentMimeTypes({ QStringLiteral("inode/directory") });
        parentColl.setRights(Akonadi::Collection::Right::CanCreateCollection);
        parentColl.setName(QStringLiteral("DecSync ") + qTypeName);
        collections << parentColl;

        QByteArray backingStore[MAX_COLLECTIONS];
        const char* names[MAX_COLLECTIONS];
        // Allocate and fill the new array with zeros so
        // decsync_list_decsync_collections can overwrite it.
        for (int i = 0; i < MAX_COLLECTIONS; ++i) {
            // decsync_list_decsync_collections needs each element to be 256
            // chars long.
            backingStore[i] = QByteArray(256, 'x');
            // Do this in two steps so the QByteArray is copied, so we get
            // different pointers to data for each one.
            backingStore[i].fill('\0');
            names[i] = backingStore[i].constData();
        }

        int collectionsFound = decsync_list_decsync_collections(
            qUtf8Printable(Settings::self()->decSyncDirectory()),
            *type, names, MAX_COLLECTIONS);
        qCDebug(log_decsyncresource, "found %d/%d collections for %s",
                collectionsFound, MAX_COLLECTIONS, *type);

        for (int i = 0; i < collectionsFound; ++i) {
            qCDebug(log_decsyncresource, "initialize %s collection %s", *type, names[i]);
            Decsync sync;
            if (int error = decsync_new(
                    &sync, qUtf8Printable(Settings::self()->decSyncDirectory()),
                    *type, names[i], this->appId)) {
                qCWarning(log_decsyncresource,
                          "failed to initialize DecSync %s collection %s: error %d",
                          *type, names[i], error);
                continue;
            }

            // TODO: Read calendar colour from static info.
            Akonadi::Collection coll;
            coll.setParentCollection(parentColl);
            coll.setRemoteId(qTypeName + QPATHSEP + QString::fromUtf8(names[i]));
            coll.setContentMimeTypes(appropriateMimetypes(*type));
            coll.setRights(Akonadi::Collection::Right::ReadOnly);

            char friendlyName[FRIENDLY_NAME_LENGTH];
            decsync_get_static_info(
                qUtf8Printable(Settings::self()->decSyncDirectory()),
                *type, names[i], "\"name\"", friendlyName, FRIENDLY_NAME_LENGTH);

            // friendlyName contains a JSON-encoded string, not the actual
            // value! Wrap it in [ ] so QJsonDocument can decode it.
            QByteArray array = QByteArray(friendlyName).prepend('[').append(']');
            coll.setName(QJsonDocument::fromJson(array).array().first().toString());

            collections << coll;

            decsync_free(sync);
        }

    }
    collectionsRetrieved(collections);
}

void onEntryUpdate(const char** path, const int len, const char* datetime,
                   const char* key, const char* value, void* extra)
{
    // value contains a JSON-encoded string, not the actual value!
    // Wrap it in [ ] so QJsonDocument can decode it.
    QByteArray arrayified = QByteArray(value).prepend('[').append(']');
    QJsonValue payload = QJsonDocument::fromJson(arrayified).array().first();
    if (payload.isNull()) {
        // This item is deleted. Do nothing.
        return;
    }

    QStringList pathComponents;
    for (int i = 0; i < len; ++i) {
        pathComponents << QString::fromUtf8(path[i]);
    }
    QString remoteId = pathComponents.join(QPATHSEP);

    qCDebug(log_decsyncresource, "got update notification: path=%s datetime=%s key=%s",
            qUtf8Printable(remoteId), datetime, key);

    ItemListAndMime* info = static_cast<ItemListAndMime*>(extra);
    Akonadi::Item item;
    item.setRemoteId(remoteId);
    item.setMimeType(info->mime);
    item.setPayloadFromData(payload.toString().toUtf8());
    info->items << item;
}

void DecSyncResource::retrieveItems(const Akonadi::Collection &collection)
{
    // This method is called when Akonadi wants to know about all the items in
    // the given collection. You can but don't have to provide all the data for
    // each item, remote ID and MIME type are enough at this stage.
    qCDebug(log_decsyncresource, "retrieveItems");

    const QList<QByteArray> components = collection.remoteId().toUtf8().split(PATHSEP);
    const char* collType = components[0].constData();
    const char* collName = components[1].constData();
    qCDebug(log_decsyncresource, "getting items for %s/%s", collType, collName);

    Decsync sync;
    if (int error = decsync_new(
            &sync, qUtf8Printable(Settings::self()->decSyncDirectory()),
            collType, collName, this->appId)) {
        Q_EMIT status(Akonadi::AgentBase::Status::Broken,
                      QStringLiteral("failed to initialize DecSync collection"));
        qCWarning(log_decsyncresource,
                  "failed to initialize DecSync %s collection %s: error %d",
                  collType, collName, error);
        return;
    }

#define PATH_LENGTH 1
    const char* path[PATH_LENGTH] { "resources" };
    decsync_add_listener(sync, path, PATH_LENGTH, onEntryUpdate);
    decsync_init_stored_entries(sync);

    Akonadi::Item::List items;
    ItemListAndMime info(items, appropriateMimetypes(collType).first());
    decsync_execute_all_stored_entries_for_path_prefix(sync, path, PATH_LENGTH, &info);
#undef PATH_LENGTH

    decsync_free(sync);
    itemsRetrieved(items);
}

/*
 * Note that these three functions don't get the full payload of the items by default,
 * you need to change the item fetch scope of the change recorder to fetch the full
 * payload. This can be expensive with big payloads, though.
 *
 * Once you have handled changes in itemAdded() and itemChanged(), call changeCommitted().
 * Once you have handled changes in itemRemoved(), call changeProcessed();
 * These methods are called whenever a local item related to this resource is
 * added, modified or deleted. They are only called if the resource is online, otherwise
 * all changes are recorded and replayed as soon the resource is online again.
 */

void DecSyncResource::itemAdded(const Akonadi::Item &item, const Akonadi::Collection &collection)
{
    Akonadi::ItemFetchJob *job = new Akonadi::ItemFetchJob(item);
    connect(job, SIGNAL(result(KJob*)), SLOT(DecSyncResource::itemAddedWithPayload(KJob*)));
    job->fetchScope().fetchFullPayload();
}

void DecSyncResource::itemAddedWithPayload(KJob* job)
{
    if (job->error()) {
        qCWarning(log_decsyncresource, "couldn't add item: fetch job error");
        return;
    }

    Akonadi::ItemFetchJob* fetchJob = qobject_cast<Akonadi::ItemFetchJob*>(job);
    const Akonadi::Item &item = fetchJob->items().first();

    const QString collectionId = collection.remoteId(),
        syncType = collectionId.section(QPATHSEP, 0, 0),
        collectionName = collectionId.section(QPATHSEP, 1, 1);

    Decsync sync;
    if (int error = decsync_new(
            &sync, qUtf8Printable(Settings::self()->decSyncDirectory()),
            qUtf8Printable(syncType), qUtf8Printable(collectionName), this->appId)) {
        qCWarning(log_decsyncresource, "failed to create DecSync instance %s/%s: error %d",
                  qUtf8Printable(syncType), qUtf8Printable(collectionName), error);
    } else {
        qCDebug(log_decsyncresource, "itemAdded with payload %s", item.payloadData().constData());
        QJsonDocument doc;
        doc.setArray({ QJsonValue(QString::fromUtf8(item.payloadData())) });
        QByteArray json = doc.toJson(QJsonDocument::Compact);
        // Remove leading '[' and trailing ']', ...
        json.remove(0, 1).chop(1);
        // ... so what's left is the JSON encoding of the new payload.

        const QByteArray itemId = item.remoteId().toUtf8();
#define PATH_LENGTH 2
        const char* path[PATH_LENGTH] { "resources", itemId.constData() };
        decsync_set_entry(sync, path, PATH_LENGTH, "null", json.constData());
#undef PATH_LENGTH

        changeCommitted(item);
    }
    decsync_free(sync);
}

void DecSyncResource::itemChanged(const Akonadi::Item &item, const QSet<QByteArray> &parts)
{
    Q_UNUSED(item);
    Q_UNUSED(parts);
}

void DecSyncResource::itemRemoved(const Akonadi::Item &item)
{
    const QString collectionId = item.parentCollection().remoteId(),
        syncType = collectionId.section(QPATHSEP, 0, 0),
        collectionName = collectionId.section(QPATHSEP, 1, 1);

    Decsync sync;
    if (int error = decsync_new(
            &sync, qUtf8Printable(Settings::self()->decSyncDirectory()),
            qUtf8Printable(syncType), qUtf8Printable(collectionName), this->appId)) {
        qCWarning(log_decsyncresource, "failed to create DecSync instance %s/%s: error %d",
                  qUtf8Printable(syncType), qUtf8Printable(collectionName), error);
    } else {
        const QByteArray itemId = item.remoteId().toUtf8();
#define PATH_LENGTH 2
        const char* path[PATH_LENGTH] { "resources", itemId.constData() };
        // To delete a contact or calendar event, set its DecSync value to JSON null.
        decsync_set_entry(sync, path, PATH_LENGTH, "null", "null");
#undef PATH_LENGTH

        changeCommitted(item);
    }
    decsync_free(sync);
}

void DecSyncResource::collectionAdded(const Akonadi::Collection &collection,
                                      const Akonadi::Collection &parent)
{
    Q_UNUSED(collection);
    Q_UNUSED(parent);
}

void DecSyncResource::collectionChanged(const Akonadi::Collection &collection,
                                        const QSet<QByteArray> &changedAttributes)
{
    QByteArray attrs;
    for (QSet<QByteArray>::const_iterator i = changedAttributes.constBegin();
         i != changedAttributes.constEnd(); ++i) {
        attrs += '/';
        attrs += *i;
    }

    qCDebug(log_decsyncresource, "collectionChanged(%p, %s)",
            qUtf8Printable(collection.remoteId()), attrs.constData());
}

void DecSyncResource::collectionRemoved(const Akonadi::Collection &collection)
{
    Q_UNUSED(collection);
}

AKONADI_RESOURCE_MAIN(DecSyncResource)
