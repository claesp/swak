#include "core/contacts.h"
#include "core/delete_confirmation.h"
#include "core/interfaces.h"
#include "core/map_tools.h"
#include "core/modal.h"
#include "core/object_sharing.h"
#include "core/text.h"
#include "core/user_objects.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace swak {
namespace {
// Lets the user choose the TAK users that receive an object.
// Returns false when the user cancels.
bool chooseRecipients(QWidget *parent,
                      const Contacts *contacts,
                      const QString &title,
                      QList<ShareRecipient> *recipients)
{
    Modal<QDialog> dialog(parent);
    dialog->setObjectName(QStringLiteral("shareRecipientsDialog"));
    dialog->setWindowTitle(QObject::tr("Share %1").arg(title));
    auto *layout = new QVBoxLayout(dialog.get());
    auto *notice = new QLabel(
        QObject::tr("Checked users receive the object through TAK Server. Other outputs, such "
                    "as multicast and LAN peers, send it to all receivers."),
        dialog.get());
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *list = new QListWidget(dialog.get());
    list->setObjectName(QStringLiteral("shareRecipients"));
    QList<ShareRecipient> available;
    if (contacts) {
        for (const auto &contact : contacts->contacts()) {
            available.append({contact.uid, contact.callsign});
        }
    }
    // Keep earlier recipients that are not in the contact list now.
    for (const auto &recipient : std::as_const(*recipients)) {
        if (std::none_of(available.cbegin(), available.cend(), [&recipient](const auto &item) {
                return item.uid == recipient.uid;
            })) {
            available.append(recipient);
        }
    }
    for (const auto &recipient : std::as_const(available)) {
        auto *item = new QListWidgetItem(
            recipient.callsign.isEmpty() ? recipient.uid : recipient.callsign, list);
        item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        item->setData(Qt::UserRole, recipient.uid);
        item->setData(Qt::UserRole + 1, recipient.callsign);
        item->setToolTip(plainToolTip(recipient.uid));
        const bool checked =
            std::any_of(recipients->cbegin(), recipients->cend(), [&recipient](const auto &r) {
                return r.uid == recipient.uid;
            });
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto *empty = new QLabel(QObject::tr("No TAK users are known. Open Contacts and select "
                                         "Refresh, or wait for position reports."),
                             dialog.get());
    empty->setObjectName(QStringLiteral("shareRecipientsEmpty"));
    empty->setWordWrap(true);
    empty->setVisible(available.isEmpty());
    layout->addWidget(empty);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.get());
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(QObject::tr("Share"));
    const auto update = [list, ok] {
        int count = 0;
        for (int row = 0; row < list->count(); ++row) {
            count += list->item(row)->checkState() == Qt::Checked ? 1 : 0;
        }
        ok->setEnabled(count > 0 && count <= ObjectSharing::MaxRecipients);
    };
    QObject::connect(list, &QListWidget::itemChanged, dialog.get(), update);
    update();
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog.get(), &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog->exec() != QDialog::Accepted || !dialog) {
        return false;
    }
    QList<ShareRecipient> chosen;
    for (int row = 0; row < list->count(); ++row) {
        const auto *item = list->item(row);
        if (item->checkState() == Qt::Checked) {
            chosen.append(
                {item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString()});
        }
    }
    *recipients = chosen;
    return true;
}
// Adds Share, Share with, and Stop sharing to the menus of local objects.
class SharingTool final : public MapTool
{
public:
    SharingTool(PluginServices &services, QObject *parent)
        : MapTool(parent), objects_(services.objects), sharing_(services.sharing),
          contacts_(services.contacts)
    {
    }
    void populateInformationMenu(const MapItem &item, QMenu &menu) override
    {
        populateContextMenu(item, menu);
    }
    void populateContextMenu(const MapItem &item, QMenu &menu) override
    {
        if (!objects_ || !sharing_ || item.owner == QStringLiteral("org.swak.events") ||
            item.owner.startsWith(QStringLiteral("org.swak.data/"))) {
            return;
        }
        const auto object = objects_->find(item.id);
        if (!object || sharedCotType(*object).isEmpty()) {
            return;
        }
        const QPointer<QWidget> parent(menu.parentWidget());
        const bool shared = sharing_->isShared(item.id);
        menu.addSeparator();
        if (shared) {
            auto *stop = menu.addAction(tr("Stop sharing"));
            stop->setObjectName(QStringLiteral("stopSharingObject"));
            stop->setToolTip(tr("Remove the object from TAK users and stop sending it."));
            connect(stop, &QAction::triggered, this, [this, id = item.id] {
                if (sharing_) {
                    sharing_->stopSharing(id);
                }
            });
        } else {
            auto *share = menu.addAction(tr("Share"));
            share->setObjectName(QStringLiteral("shareObject"));
            share->setToolTip(tr("Send the object to the channels of this user."));
            connect(share, &QAction::triggered, this, [this, parent, id = item.id] {
                if (sharing_) {
                    showError(parent, sharing_->share(id));
                }
            });
        }
        auto *with = menu.addAction(tr("Share with ..."));
        with->setObjectName(QStringLiteral("shareObjectWith"));
        with->setToolTip(tr("Send the object only to chosen TAK users."));
        connect(
            with, &QAction::triggered, this, [this, parent, id = item.id, title = object->title] {
                // Open the dialog after the menu closes, so the dialog does not run inside it.
                QMetaObject::invokeMethod(
                    this,
                    [this, parent, id, title] {
                        if (!sharing_) {
                            return;
                        }
                        auto recipients = sharing_->recipients(id);
                        // The modal dialog runs the event loop, so the service can die in it.
                        // cppcheck-suppress knownConditionTrueFalse
                        if (chooseRecipients(parent, contacts_, title, &recipients) && sharing_) {
                            showError(parent, sharing_->share(id, recipients));
                        }
                    },
                    Qt::QueuedConnection);
            });
    }

private:
    static void showError(QWidget *parent, const QString &error)
    {
        if (!error.isEmpty()) {
            Modal<QMessageBox> box(
                QMessageBox::Warning, tr("Share"), error, QMessageBox::Ok, parent);
            box->setObjectName(QStringLiteral("shareError"));
            box->exec();
        }
    }
    QPointer<UserObjects> objects_;
    QPointer<ObjectSharing> sharing_;
    QPointer<Contacts> contacts_;
};
} // namespace
class SharingPlugin final : public QObject, public Plugin, public MenuPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "sharing/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MenuPlugin)
public:
    QString name() const override
    {
        return tr("Sharing");
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        auto *sharing = services.sharing;
        if (!sharing) {
            return;
        }
        if (services.mapTools) {
            services.mapTools->add(new SharingTool(services, &menu));
        }
        auto *automatic = menu.addAction(tr("Share New Objects Automatically"));
        automatic->setObjectName(QStringLiteral("shareAutomatically"));
        automatic->setCheckable(true);
        automatic->setToolTip(tr("Share each new marker, circle, line, area, and route."));
        const auto update = [automatic, sharing] {
            automatic->setChecked(sharing->automatic());
        };
        update();
        connect(sharing, &ObjectSharing::changed, automatic, update);
        connect(automatic, &QAction::toggled, sharing, &ObjectSharing::setAutomatic);
    }
};
} // namespace swak
#include "sharing.moc"

#include <algorithm>
