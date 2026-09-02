#pragma once

#include "OBSQTDisplay.hpp"

#include <obs.hpp>

#include <QMouseEvent>

#include <mutex>
#include <utility>
#include <vector>

class DReXMultiCanvasPreview : public OBSQTDisplay {
	Q_OBJECT

public:
	explicit DReXMultiCanvasPreview(QWidget *parent = nullptr);
	~DReXMultiCanvasPreview() override;

	void SetCanvases(std::vector<OBSCanvasAutoRelease> newCanvases);
	void SetVisibleCanvasCount(size_t count);

signals:
	void CanvasSelected(size_t index);

protected:
	void mousePressEvent(QMouseEvent *event) override;

private:
	static void Render(void *data, uint32_t width, uint32_t height);
	void Draw(uint32_t width, uint32_t height);
	std::pair<size_t, size_t> GridDimensions() const;

	std::mutex canvasesMutex;
	std::vector<OBSCanvasAutoRelease> canvases;
	size_t visibleCanvasCount = 0;
	size_t selectedCanvas = 0;
};
