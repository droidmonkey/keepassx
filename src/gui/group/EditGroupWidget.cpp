/*
 *  Copyright (C) 2011 Felix Geyer <debfx@fobos.de>
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

#include "EditGroupWidget.h"
#include "ui_EditGroupWidgetMain.h"

#include "core/Metadata.h"
#include "core/FilePath.h"
#include "core/Tools.h"
#include "gui/EditWidgetIcons.h"
#include "gui/EditWidgetAutoType.h"
#include "gui/EditWidgetProperties.h"

EditGroupWidget::EditGroupWidget(QWidget* parent)
    : EditWidget(parent)
    , m_mainUi(new Ui::EditGroupWidgetMain())
    , m_editGroupWidgetMain(new QWidget())
    , m_editGroupWidgetIcons(new EditWidgetIcons())
    , m_editGroupWidgetAutoType(new EditWidgetAutoType())
    , m_editWidgetProperties(new EditWidgetProperties())
    , m_group(nullptr)
    , m_database(nullptr)
{
    m_mainUi->setupUi(m_editGroupWidgetMain);

    addPage(tr("Group"), FilePath::instance()->icon("actions", "document-edit"), m_editGroupWidgetMain);
    addPage(tr("Icon"), FilePath::instance()->icon("apps", "preferences-desktop-icons"), m_editGroupWidgetIcons);
    addPage(tr("Auto-Type"), FilePath::instance()->icon("actions", "key-enter"), m_editGroupWidgetAutoType);
    addPage(tr("Properties"), FilePath::instance()->icon("actions", "document-properties"), m_editWidgetProperties);

    connect(m_mainUi->expireCheck, SIGNAL(toggled(bool)), m_mainUi->expireDatePicker, SLOT(setEnabled(bool)));

    connect(this, SIGNAL(apply()), SLOT(apply()));
    connect(this, SIGNAL(accepted()), SLOT(save()));
    connect(this, SIGNAL(rejected()), SLOT(cancel()));

    connect(m_editGroupWidgetIcons, SIGNAL(messageEditEntry(QString, MessageWidget::MessageType)),
            SLOT(showMessage(QString, MessageWidget::MessageType)));
    connect(m_editGroupWidgetIcons, SIGNAL(messageEditEntryDismiss()), SLOT(hideMessage()));
}

EditGroupWidget::~EditGroupWidget()
{
}

void EditGroupWidget::loadGroup(Group *group, bool create, Database* database)
{
    m_group = group;
    m_database = database;

    if (create) {
        setHeadline(tr("Add group"));
    }
    else {
        setHeadline(tr("Edit group"));
    }

    const bool parentSearchingEnabled = group->parentGroup() ? group->parentGroup()->resolveSearchingEnabled() : true;
    m_mainUi->searchComboBox->addTriStateItems(parentSearchingEnabled);

    m_mainUi->editName->setText(group->name());
    m_mainUi->editNotes->setPlainText(group->notes());
    m_mainUi->expireCheck->setChecked(group->timeInfo().expires());
    m_mainUi->expireDatePicker->setDateTime(group->timeInfo().expiryTime().toLocalTime());
    m_mainUi->searchComboBox->setCurrentIndex(Tools::indexFromTriState(group->searchingEnabled()));

    IconStruct iconStruct;
    iconStruct.uuid = group->iconUuid();
    iconStruct.number = group->iconNumber();
    m_editGroupWidgetIcons->load(group->uuid(), database, iconStruct);

    const bool parentAutoTypeEnabled = group->parentGroup() ? group->parentGroup()->resolveAutoTypeEnabled() : true;
    // TODO: frostasm - add autoTypeAssociations
    m_editGroupWidgetAutoType->setFields(group->autoTypeEnabled(), parentAutoTypeEnabled,
                                         group->defaultAutoTypeSequence(), group->effectiveAutoTypeSequence(),
                                         nullptr);
    m_editWidgetProperties->setFields(group->timeInfo(), group->uuid());

    setCurrentPage(0);

    m_mainUi->editName->setFocus();
}

void EditGroupWidget::save()
{
    apply();
    clear();
    emit editFinished(true);
}

void EditGroupWidget::apply()
{
    m_group->setName(m_mainUi->editName->text());
    m_group->setNotes(m_mainUi->editNotes->toPlainText());
    m_group->setExpires(m_mainUi->expireCheck->isChecked());
    m_group->setExpiryTime(m_mainUi->expireDatePicker->dateTime().toUTC());

    m_group->setSearchingEnabled(Tools::triStateFromIndex(m_mainUi->searchComboBox->currentIndex()));
    m_group->setAutoTypeEnabled(m_editGroupWidgetAutoType->autoTypeEnabled());


    if (m_editGroupWidgetAutoType->inheritSequenceEnabled()) {
        m_group->setDefaultAutoTypeSequence(QString());
    }
    else {
        m_group->setDefaultAutoTypeSequence(m_editGroupWidgetAutoType->sequence());
    }

    IconStruct iconStruct = m_editGroupWidgetIcons->state();

    if (iconStruct.number < 0) {
        m_group->setIcon(Group::DefaultIconNumber);
    }
    else if (iconStruct.uuid.isNull()) {
        m_group->setIcon(iconStruct.number);
    }
    else {
        m_group->setIcon(iconStruct.uuid);
    }
}

void EditGroupWidget::cancel()
{
    if (!m_group->iconUuid().isNull() &&
            !m_database->metadata()->containsCustomIcon(m_group->iconUuid())) {
        m_group->setIcon(Entry::DefaultIconNumber);
    }

    clear();
    emit editFinished(false);
}

void EditGroupWidget::clear()
{
    m_group = nullptr;
    m_database = nullptr;
    m_editGroupWidgetIcons->reset();
}

void EditGroupWidget::addTriStateItems(QComboBox* comboBox, bool inheritDefault)
{
    QString inheritDefaultString;
    if (inheritDefault) {
        inheritDefaultString = tr("Enable");
    }
    else {
        inheritDefaultString = tr("Disable");
    }

    comboBox->clear();
    comboBox->addItem(tr("Inherit from parent group (%1)").arg(inheritDefaultString));
    comboBox->addItem(tr("Enable"));
    comboBox->addItem(tr("Disable"));
}
