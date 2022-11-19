/* Copyright 2013-2022 MultiMC Contributors
 * Copyright 2021-2022 Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "TechnicPage.h"
#include "ui_TechnicPage.h"

#include <QKeyEvent>

#include "ui/dialogs/NewInstanceDialog.h"

#include "BuildConfig.h"
#include "TechnicModel.h"
#include "modplatform/technic/SingleZipPackInstallTask.h"
#include "modplatform/technic/SolderPackInstallTask.h"
#include "modplatform/technic/SolderPackManifest.h"
#include "Json.h"

#include "Application.h"

TechnicPage::TechnicPage(NewInstanceDialog* dialog, QWidget *parent)
    : QWidget(parent), ui(new Ui::TechnicPage), dialog(dialog)
{
    ui->setupUi(this);
    connect(ui->searchButton, &QPushButton::clicked, this, &TechnicPage::triggerSearch);
    ui->searchEdit->installEventFilter(this);
    model = new Technic::ListModel(this);
    ui->packView->setModel(model);

    connect(ui->packView->selectionModel(), &QItemSelectionModel::currentChanged, this, &TechnicPage::onSelectionChanged);
    connect(ui->versionSelectionBox, &QComboBox::currentTextChanged, this, &TechnicPage::onVersionSelectionChanged);
}

bool TechnicPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == ui->searchEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return) {
            triggerSearch();
            keyEvent->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

TechnicPage::~TechnicPage()
{
    delete ui;
}

bool TechnicPage::shouldDisplay() const
{
    return true;
}

void TechnicPage::openedImpl()
{
    suggestCurrent();
    triggerSearch();
}

void TechnicPage::triggerSearch() {
    model->searchWithTerm(ui->searchEdit->text());
}

void TechnicPage::onSelectionChanged(QModelIndex first, QModelIndex second)
{
    ui->versionSelectionBox->clear();

    if(!first.isValid())
    {
        if(isOpened)
        {
            dialog->setSuggestedPack();
        }
        return;
    }

    current = model->data(first, Qt::UserRole).value<Technic::Modpack>();
    suggestCurrent();
}

void TechnicPage::suggestCurrent()
{
    if (!isOpened)
    {
        return;
    }
    if (current.broken)
    {
        dialog->setSuggestedPack();
        return;
    }

    QString editedLogoName = "technic_" + current.logoName.section(".", 0, 0);
    model->getLogo(current.logoName, current.logoUrl, [this, editedLogoName](QString logo)
    {
        dialog->setSuggestedIconFromFile(logo, editedLogoName);
    });

    if (current.metadataLoaded)
    {
        metadataLoaded();
        return;
    }

    NetJob *netJob = new NetJob(QString("Technic::PackMeta(%1)").arg(current.name), APPLICATION->network());
    QString slug = current.slug;
    netJob->addNetAction(Net::Download::makeByteArray(QString("%1modpack/%2?build=%3").arg(BuildConfig.TECHNIC_API_BASE_URL, slug, BuildConfig.TECHNIC_API_BUILD), &response));
    QObject::connect(netJob, &NetJob::succeeded, this, [this, slug]
    {
        jobPtr.reset();

        if (current.slug != slug)
        {
            return;
        }

        QJsonParseError parse_error {};
        QJsonDocument doc = QJsonDocument::fromJson(response, &parse_error);
        QJsonObject obj = doc.object();
        if(parse_error.error != QJsonParseError::NoError)
        {
            qWarning() << "Error while parsing JSON response from Technic at " << parse_error.offset << " reason: " << parse_error.errorString();
            qWarning() << *response;
            return;
        }
        if (!obj.contains("url"))
        {
            qWarning() << "Json doesn't contain an url key";
            return;
        }
        QJsonValueRef url = obj["url"];
        if (url.isString())
        {
            current.url = url.toString();
        }
        else
        {
            if (!obj.contains("solder"))
            {
                qWarning() << "Json doesn't contain a valid url or solder key";
                return;
            }
            QJsonValueRef solderUrl = obj["solder"];
            if (solderUrl.isString())
            {
                current.url = solderUrl.toString();
                current.isSolder = true;
            }
            else
            {
                qWarning() << "Json doesn't contain a valid url or solder key";
                return;
            }
        }

        current.minecraftVersion = Json::ensureString(obj, "minecraft", QString(), "__placeholder__");
        current.websiteUrl = Json::ensureString(obj, "platformUrl", QString(), "__placeholder__");
        current.author = Json::ensureString(obj, "user", QString(), "__placeholder__");
        current.description = Json::ensureString(obj, "description", QString(), "__placeholder__");
        current.currentVersion = Json::ensureString(obj, "version", QString(), "__placeholder__");
        current.metadataLoaded = true;

        metadataLoaded();
    });

    jobPtr = netJob;
    jobPtr->start();
}

// expects current.metadataLoaded to be true
void TechnicPage::metadataLoaded()
{
    QString text = "";
    QString name = current.name;

    if (current.websiteUrl.isEmpty())
        text = name.toHtmlEscaped();
    else
        text = "<a href=\"" + current.websiteUrl.toHtmlEscaped() + "\">" + name.toHtmlEscaped() + "</a>";

    if (!current.author.isEmpty()) {
        text += "<br>" + tr(" by ") + current.author.toHtmlEscaped();
    }

    text += "<br><br>";

    ui->packDescription->setHtml(text + current.description);

    // Strip trailing forward-slashes from Solder URL's
    if (current.isSolder) {
        while (current.url.endsWith('/')) current.url.chop(1);
    }

    // Display versions from Solder
    if (!current.isSolder) {
        // If the pack isn't a Solder pack, it only has the single version
        ui->versionSelectionBox->addItem(current.currentVersion);
    }
    else if (current.versionsLoaded) {
        // reverse foreach, so that the newest versions are first
        for (auto i = current.versions.size(); i--;) {
            ui->versionSelectionBox->addItem(current.versions.at(i));
        }
        ui->versionSelectionBox->setCurrentText(current.recommended);
    }
    else {
        // For now, until the versions are pulled from the Solder instance, display the current
        // version so we can display something quicker
        ui->versionSelectionBox->addItem(current.currentVersion);

        auto* netJob = new NetJob(QString("Technic::SolderMeta(%1)").arg(current.name), APPLICATION->network());
        auto url = QString("%1/modpack/%2").arg(current.url, current.slug);
        netJob->addNetAction(Net::Download::makeByteArray(QUrl(url), &response));

        QObject::connect(netJob, &NetJob::succeeded, this, &TechnicPage::onSolderLoaded);

        jobPtr = netJob;
        jobPtr->start();
    }

    selectVersion();
}

void TechnicPage::selectVersion() {
    if (!isOpened) {
        return;
    }
    if (current.broken) {
        dialog->setSuggestedPack();
        return;
    }

    if (!current.isSolder)
    {
        dialog->setSuggestedPack(current.name + " " + selectedVersion, new Technic::SingleZipPackInstallTask(current.url, current.minecraftVersion));
    }
    else
    {
        dialog->setSuggestedPack(current.name + " " + selectedVersion, new Technic::SolderPackInstallTask(APPLICATION->network(), current.url + "/modpack/" + current.slug, selectedVersion, current.minecraftVersion));
    }
}

void TechnicPage::onSolderLoaded() {
    jobPtr.reset();

    auto fallback = [this]() {
        current.versionsLoaded = true;

        current.versions.clear();
        current.versions.append(current.currentVersion);
    };

    current.versions.clear();

    QJsonParseError parse_error {};
    auto doc = QJsonDocument::fromJson(response, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        qWarning() << "Error while parsing JSON response from Solder at " << parse_error.offset << " reason: " << parse_error.errorString();
        qWarning() << response;
        fallback();
        return;
    }
    auto obj = doc.object();

    TechnicSolder::Pack pack;
    try {
        TechnicSolder::loadPack(pack, obj);
    }
    catch (const JSONValidationError &err) {
        qCritical() << "Couldn't parse Solder pack metadata:" << err.cause();
        fallback();
        return;
    }

    current.versionsLoaded = true;
    current.recommended = pack.recommended;
    current.versions << pack.builds;

    // Finally, let's reload :)
    ui->versionSelectionBox->clear();
    metadataLoaded();
}

void TechnicPage::onVersionSelectionChanged(QString data) {
    if (data.isNull() || data.isEmpty()) {
        selectedVersion = "";
        return;
    }

    selectedVersion = data;
    selectVersion();
}
