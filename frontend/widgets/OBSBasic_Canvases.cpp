/******************************************************************************
    Copyright (C) 2025 by Dennis Sädtler <saedtler@twitch.tv>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <widgets/DReXMultiCanvasPreview.hpp>

#include <QSignalBlocker>

#include <unordered_set>

namespace {
struct DReXMediaSources {
	std::unordered_set<obs_source_t *> seen;
	std::vector<OBSSource> sources;
};

bool CollectDReXMediaSource(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto &media = *static_cast<DReXMediaSources *>(data);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) {
		return true;
	}

	if ((obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA) && media.seen.emplace(source).second) {
		media.sources.emplace_back(source);
	}

	if (obs_scene_t *nestedScene = obs_group_or_scene_from_source(source)) {
		obs_scene_enum_items(nestedScene, CollectDReXMediaSource, data);
	}
	return true;
}
} // namespace

void OBSBasic::CanvasRemoved(void *data, calldata_t *params)
{
	obs_canvas_t *canvas = static_cast<obs_canvas_t *>(calldata_ptr(params, "canvas"));
	QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::RemoveCanvas, canvas);
}

const OBS::Canvas &OBSBasic::AddCanvas(const std::string &name, obs_video_info *ovi, int flags)
{
	OBSCanvas canvas = obs_canvas_create(name.c_str(), ovi, flags);
	auto &it = canvases.emplace_back(canvas);
	OnEvent(OBS_FRONTEND_EVENT_CANVAS_ADDED);
	return it;
}

bool OBSBasic::RemoveCanvas(OBSCanvas canvas)
{
	bool removed = false;
	if (!canvas) {
		return removed;
	}

	auto canvas_it = std::find(std::begin(canvases), std::end(canvases), canvas);
	if (canvas_it != std::end(canvases)) {
		// Move canvas to a temporary object to delay removal of the canvas and calls to its signal handlers
		// until after erase() completes. This is to avoid issues with recursion coming from the
		// CanvasRemoved() signal handler.
		OBS::Canvas tmp = std::move(*canvas_it);
		canvases.erase(canvas_it);
		removed = true;
	}

	if (removed) {
		OnEvent(OBS_FRONTEND_EVENT_CANVAS_REMOVED);
	}

	return removed;
}

void OBSBasic::ClearCanvases()
{
	drexCanvasSignals.clear();
	drexRegisteredScenes.clear();
	// Delete canvases one-by-one to ensure OBS_FRONTEND_EVENT_CANVAS_REMOVED is sent for each
	while (!canvases.empty()) {
		RemoveCanvas(OBSCanvas(canvases.back()));
	}
}

void OBSBasic::SetDReXMultiCanvasCount(size_t count)
{
	if (!drexMultiCanvasPreview) {
		return;
	}

	if (!count) {
		if (drexVisibleCanvasCount) {
			SelectDReXCanvas(0);
		}
		drexVisibleCanvasCount = 0;
		drexEditingScene = nullptr;
		drexMediaControls->setVisible(false);
		drexCanvasDock->setVisible(false);
		drexCanvasList->clear();
		drexCanvasSelector->clear();
		ui->contextContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		if (obs_display_t *display = drexMultiCanvasPreview->GetDisplay()) {
			obs_display_set_enabled(display, false);
		}
		drexMultiCanvasPreview->setVisible(false);
		EnablePreviewDisplay(previewEnabled);
		return;
	}

	count = std::min<size_t>(count, 10);
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "DReX Multi-Canvas: video is not initialized");
		return;
	}

	while (canvases.size() + 1 < count) {
		const size_t number = canvases.size() + 2;
		const std::string canvasName = "Canvas " + std::to_string(number);
		const auto &canvas = AddCanvas(canvasName, &ovi, ACTIVATE | SCENE_REF);
		const std::string sceneName = canvasName + " Scene";
		obs_scene_t *scene = obs_canvas_scene_create(canvas, sceneName.c_str());
		if (scene) {
			obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
			RegisterDReXCanvasScene(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
	}

	std::vector<OBSCanvasAutoRelease> visibleCanvases;
	visibleCanvases.reserve(count);
	visibleCanvases.emplace_back(obs_get_main_canvas());

	for (size_t index = 0; index + 1 < count && index < canvases.size(); ++index) {
		obs_canvas_t *canvas = canvases[index];
		if (!obs_canvas_has_video(canvas) && !obs_canvas_reset_video(canvas, &ovi)) {
			blog(LOG_WARNING, "DReX Multi-Canvas: failed to initialize canvas '%s'",
			     obs_canvas_get_name(canvas));
		}

		OBSSourceAutoRelease canvasScene = obs_canvas_get_channel(canvas, 0);
		if (!canvasScene || !obs_scene_from_source(canvasScene)) {
			const std::string sceneName = "Canvas " + std::to_string(index + 2) + " Scene";
			obs_scene_t *replacementScene = obs_canvas_scene_create(canvas, sceneName.c_str());
			if (replacementScene) {
				obs_canvas_set_channel(canvas, 0, obs_scene_get_source(replacementScene));
				canvasScene = obs_source_get_ref(obs_scene_get_source(replacementScene));
				obs_scene_release(replacementScene);
				blog(LOG_INFO, "DReX Multi-Canvas: created missing backing scene for canvas %zu",
				     index + 2);
			}
		}
		if (canvasScene && obs_scene_from_source(canvasScene)) {
			RegisterDReXCanvasScene(canvasScene);
			QListWidgetItem *listedItem = nullptr;
			for (int row = 0; row < ui->scenes->count(); ++row) {
				OBSScene listedScene = GetOBSRef<OBSScene>(ui->scenes->item(row));
				if (listedScene && obs_scene_get_source(listedScene) == canvasScene) {
					listedItem = ui->scenes->item(row);
					break;
				}
			}
			if (listedItem) {
				delete listedItem;
			}
		}
		visibleCanvases.emplace_back(obs_canvas_get_ref(canvas));
	}

	drexVisibleCanvasCount = visibleCanvases.size();
	drexMediaControls->setVisible(true);
	drexCanvasList->clear();
	drexCanvasSelector->clear();
	for (size_t index = 0; index < drexVisibleCanvasCount; ++index) {
		const QString name = QStringLiteral("Canvas %1").arg(index + 1);
		drexCanvasList->addItem(name);
		drexCanvasSelector->addItem(name);
	}
	drexCanvasDock->setVisible(true);
	ui->contextContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	drexMultiCanvasPreview->SetCanvases(std::move(visibleCanvases));
	drexMultiCanvasPreview->SetVisibleCanvasCount(drexVisibleCanvasCount);
	ui->previewContainer->setVisible(false);
	ui->previewDisabledWidget->setVisible(false);
	drexMultiCanvasPreview->setVisible(true);
	if (obs_display_t *display = ui->preview->GetDisplay()) {
		obs_display_set_enabled(display, false);
	}
	if (obs_display_t *display = drexMultiCanvasPreview->GetDisplay()) {
		obs_display_set_enabled(display, true);
	}
	SelectDReXCanvas(0);

	blog(LOG_INFO, "DReX Multi-Canvas: showing %zu canvases", drexVisibleCanvasCount);
}

void OBSBasic::RegisterDReXCanvasScene(obs_source_t *source)
{
	for (const OBSWeakSource &weak : drexRegisteredScenes) {
		if (OBSGetStrongRef(weak) == source) {
			return;
		}
	}

	drexRegisteredScenes.emplace_back(OBSGetWeakRef(source));
	signal_handler_t *handler = obs_source_get_signal_handler(source);
	drexCanvasSignals.emplace_back(std::make_shared<OBSSignal>(handler, "item_add", OBSBasic::SceneItemAdded, this));
	drexCanvasSignals.emplace_back(std::make_shared<OBSSignal>(handler, "reorder", OBSBasic::SceneReordered, this));
	drexCanvasSignals.emplace_back(std::make_shared<OBSSignal>(handler, "refresh", OBSBasic::SceneRefreshed, this));
}

void OBSBasic::ControlDReXCanvasMedia(DReXMediaAction action)
{
	DReXMediaSources media;
	auto collectScene = [&media](obs_source_t *source) {
		if (obs_scene_t *scene = obs_scene_from_source(source)) {
			obs_scene_enum_items(scene, CollectDReXMediaSource, &media);
		}
	};

	OBSSource mainScene = OBSGetStrongRef(drexMainCanvasScene);
	if (!mainScene) {
		OBSScene current = GetCurrentScene();
		if (current) {
			obs_source_t *source = obs_scene_get_source(current);
			OBSCanvasAutoRelease canvas = obs_source_get_canvas(source);
			OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
			if (canvas == mainCanvas) {
				mainScene = source;
			}
		}
	}
	if (mainScene) {
		collectScene(mainScene);
	}

	for (size_t index = 0; index + 1 < drexVisibleCanvasCount && index < canvases.size(); ++index) {
		OBSSourceAutoRelease scene = obs_canvas_get_channel(canvases[index], 0);
		if (scene) {
			collectScene(scene);
		}
	}

	for (const OBSSource &source : media.sources) {
		switch (action) {
		case DReXMediaAction::Restart:
			obs_source_media_restart(source);
			break;
		case DReXMediaAction::Play:
			obs_source_media_play_pause(source, false);
			break;
		case DReXMediaAction::Pause:
			obs_source_media_play_pause(source, true);
			break;
		case DReXMediaAction::Stop:
			obs_source_media_stop(source);
			break;
		default:
			break;
		}
	}

	blog(LOG_INFO, "DReX Multi-Canvas: media action %d sent to %zu unique sources", int(action),
	     media.sources.size());
}

void OBSBasic::SelectDReXCanvas(size_t index)
{
	if (!drexVisibleCanvasCount || index >= drexVisibleCanvasCount) {
		return;
	}

	{
		QSignalBlocker blocker(drexCanvasList);
		drexCanvasList->setCurrentRow(int(index));
	}
	{
		QSignalBlocker blocker(drexCanvasSelector);
		drexCanvasSelector->setCurrentIndex(int(index));
	}

	OBSSource selectedScene;
	if (!index) {
		selectedScene = OBSGetStrongRef(drexMainCanvasScene);
		if (!selectedScene) {
			OBSScene current = GetCurrentScene();
			if (current) {
				obs_source_t *source = obs_scene_get_source(current);
				OBSCanvasAutoRelease canvas = obs_source_get_canvas(source);
				OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
				if (canvas == mainCanvas) {
					selectedScene = source;
					drexMainCanvasScene = OBSGetWeakRef(source);
				}
			}
		}
	} else {
		if (index - 1 >= canvases.size()) {
			return;
		}

		OBSScene current = GetCurrentScene();
		if (current) {
			obs_source_t *currentSource = obs_scene_get_source(current);
			OBSCanvasAutoRelease currentCanvas = obs_source_get_canvas(currentSource);
			OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
			if (currentCanvas == mainCanvas) {
				drexMainCanvasScene = OBSGetWeakRef(currentSource);
			}
		}

		OBSSourceAutoRelease sceneSource = obs_canvas_get_channel(canvases[index - 1], 0);
		selectedScene = sceneSource.Get();
	}

	if (selectedScene && obs_scene_from_source(selectedScene)) {
		drexEditingScene = obs_scene_from_source(selectedScene);
		currentScene = drexEditingScene;
		ui->sources->RefreshItems();
		UpdateContextBar(true);
		blog(LOG_INFO, "DReX Multi-Canvas: editing canvas %zu with scene '%s'", index + 1,
		     obs_source_get_name(selectedScene));
	}
}
