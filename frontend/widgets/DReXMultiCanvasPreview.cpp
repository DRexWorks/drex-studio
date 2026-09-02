#include "DReXMultiCanvasPreview.hpp"

#include <algorithm>

DReXMultiCanvasPreview::DReXMultiCanvasPreview(QWidget *parent) : OBSQTDisplay(parent)
{
	setMinimumSize(32, 32);
	setFocusPolicy(Qt::ClickFocus);

	connect(this, &OBSQTDisplay::DisplayCreated, this, [this](OBSQTDisplay *display) {
		obs_display_add_draw_callback(display->GetDisplay(), DReXMultiCanvasPreview::Render, this);
	});
}

DReXMultiCanvasPreview::~DReXMultiCanvasPreview()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), DReXMultiCanvasPreview::Render, this);
	}
}

void DReXMultiCanvasPreview::SetCanvases(std::vector<OBSCanvasAutoRelease> newCanvases)
{
	const std::lock_guard lock(canvasesMutex);
	canvases = std::move(newCanvases);
	visibleCanvasCount = std::min(visibleCanvasCount, canvases.size());
	selectedCanvas = std::min(selectedCanvas, visibleCanvasCount ? visibleCanvasCount - 1 : 0);
}

void DReXMultiCanvasPreview::SetVisibleCanvasCount(size_t count)
{
	const std::lock_guard lock(canvasesMutex);
	visibleCanvasCount = std::min(count, canvases.size());
	selectedCanvas = std::min(selectedCanvas, visibleCanvasCount ? visibleCanvasCount - 1 : 0);
}

std::pair<size_t, size_t> DReXMultiCanvasPreview::GridDimensions() const
{
	if (visibleCanvasCount <= 2) {
		return {2, 1};
	}
	if (visibleCanvasCount <= 4) {
		return {2, 2};
	}
	if (visibleCanvasCount <= 8) {
		return {4, 2};
	}
	return {5, 2};
}

void DReXMultiCanvasPreview::Render(void *data, uint32_t width, uint32_t height)
{
	static_cast<DReXMultiCanvasPreview *>(data)->Draw(width, height);
}

void DReXMultiCanvasPreview::Draw(uint32_t width, uint32_t height)
{
	const std::lock_guard lock(canvasesMutex);
	if (!visibleCanvasCount || !width || !height) {
		return;
	}

	const auto [columns, rows] = GridDimensions();
	const uint32_t gap = 4;
	const uint32_t tileWidth = (width - gap * (uint32_t(columns) + 1)) / uint32_t(columns);
	const uint32_t tileHeight = (height - gap * (uint32_t(rows) + 1)) / uint32_t(rows);

	gs_viewport_push();
	gs_projection_push();

	for (size_t index = 0; index < visibleCanvasCount; ++index) {
		obs_canvas_t *canvas = canvases[index];
		obs_video_info ovi = {};
		if (!canvas || !obs_canvas_get_video_info(canvas, &ovi) || !ovi.base_width || !ovi.base_height) {
			continue;
		}

		const uint32_t column = uint32_t(index % columns);
		const uint32_t row = uint32_t(index / columns);
		const uint32_t tileX = gap + column * (tileWidth + gap);
		const uint32_t tileY = gap + row * (tileHeight + gap);

		const float scale =
			std::min(float(tileWidth) / float(ovi.base_width), float(tileHeight) / float(ovi.base_height));
		const uint32_t renderWidth = std::max(1u, uint32_t(float(ovi.base_width) * scale));
		const uint32_t renderHeight = std::max(1u, uint32_t(float(ovi.base_height) * scale));
		const uint32_t renderX = tileX + (tileWidth - renderWidth) / 2;
		const uint32_t renderY = tileY + (tileHeight - renderHeight) / 2;

		gs_set_viewport(renderX, renderY, renderWidth, renderHeight);
		gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height), -100.0f, 100.0f);
		obs_render_canvas_texture_src_color_only(canvas);
	}

	gs_projection_pop();
	gs_viewport_pop();
}

void DReXMultiCanvasPreview::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		OBSQTDisplay::mousePressEvent(event);
		return;
	}

	size_t index = 0;
	{
		const std::lock_guard lock(canvasesMutex);
		if (!visibleCanvasCount || width() <= 0 || height() <= 0) {
			return;
		}

		const QPoint localPosition = mapFromGlobal(event->globalPosition().toPoint());
		const auto [columns, rows] = GridDimensions();
		const size_t column =
			std::min(size_t(localPosition.x() * double(columns) / double(width())), columns - 1);
		const size_t row =
			std::min(size_t(localPosition.y() * double(rows) / double(height())), rows - 1);
		index = row * columns + column;
		if (index >= visibleCanvasCount) {
			return;
		}

		selectedCanvas = index;
	}

	emit CanvasSelected(index);
}
