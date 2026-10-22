/*
GazCast Clean Record for OBS Studio
Copyright (C) 2026 GazCast

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Clean recording.
 *
 * How it works:
 * - Sources to leave out carry the "Hide in GazCast clean recording" filter
 *   (gazcast_clean_hide). Overlay-type sources (browser, image, slideshow,
 *   text, and GazCast's own) get it automatically the first time they are
 *   seen; any source can be ticked or unticked in the dock's list or its
 *   Filters window. The
 *   filter is saved with the scene collection like any other filter.
 * - While a clean recording runs, a private source (gazcast_clean_mix) renders
 *   OBS's program output (output channel 0, i.e. the transition) into its own
 *   view/video mix. During that render a flag is set, and the filter draws
 *   nothing. Everywhere else (stream, normal recording, preview, projectors)
 *   the filter passes the source through untouched.
 * - The mix gets its own encoders (copies of the normal recording's encoder
 *   settings when available) and an ffmpeg_muxer output, with the same audio
 *   as the normal recording (its first track).
 *
 * Known limit: OBS renders some things once per frame and reuses them (a
 * scene item with crop, a scale filter or a blending mode set; a transition
 * while it is running). The main output renders first, so an overlay inside
 * such an item, and any overlay during a transition, still shows in the clean
 * copy. The stream is never affected.
 */

#include "clean-record.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <plugin-support.h>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char *kMixSourceId = "gazcast_clean_mix";
constexpr const char *kHideFilterId = "gazcast_clean_hide";
constexpr const char *kDockId = "gazcast-clean";
/* Private setting on a source: GazCast already tagged it once, so a user who
 * removes the filter on purpose does not get it back. */
constexpr const char *kAutoTagged = "gazcast_clean_tagged";
constexpr const char *kConfigFile = "clean-record.json";

/* > 0 while the clean mix renders. Rendering only happens on OBS's graphics
 * thread, so a plain counter is enough. */
int g_cleanPass = 0;

/* ------------------------------------------------------------------------- */
/* Source types */

const char *MixGetName(void *)
{
	return "GazCast clean mix";
}

void *MixCreate(obs_data_t *, obs_source_t *source)
{
	return source;
}

void MixDestroy(void *) {}

uint32_t MixGetWidth(void *)
{
	obs_video_info ovi;
	return obs_get_video_info(&ovi) ? ovi.base_width : 0;
}

uint32_t MixGetHeight(void *)
{
	obs_video_info ovi;
	return obs_get_video_info(&ovi) ? ovi.base_height : 0;
}

void MixRender(void *, gs_effect_t *)
{
	obs_source_t *program = obs_get_output_source(0);
	if (!program) {
		return;
	}

	g_cleanPass++;
	obs_source_video_render(program);
	g_cleanPass--;

	obs_source_release(program);
}

const char *HideGetName(void *)
{
	return obs_module_text("GazCast.Clean.FilterName");
}

void *HideCreate(obs_data_t *, obs_source_t *source)
{
	return source;
}

void HideDestroy(void *) {}

void HideRender(void *data, gs_effect_t *)
{
	if (g_cleanPass > 0) {
		return;
	}
	obs_source_skip_video_filter(static_cast<obs_source_t *>(data));
}

/* ------------------------------------------------------------------------- */
/* Which sources are left out */

bool IsGazCastOverlay(obs_source_t *source)
{
	const char *name = obs_source_get_name(source);
	if (name && strncmp(name, "GazCast", 7) == 0) {
		return true;
	}

	if (strcmp(obs_source_get_unversioned_id(source), "browser_source") != 0) {
		return false;
	}

	/* The app adds overlays as http://127.0.0.1:5510/o/<key>; the web app
	 * serves them from overlays.gazgoc.com.ng. */
	static const QRegularExpression localOverlay(QStringLiteral(R"(^https?://(127\.0\.0\.1|localhost)(:\d+)?/o/)"),
						     QRegularExpression::CaseInsensitiveOption);
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	const QString url = QString::fromUtf8(obs_data_get_string(settings, "url"));
	return url.contains(localOverlay) || url.contains(QLatin1String("gazgoc.com.ng"), Qt::CaseInsensitive);
}

/* Graphics laid over the show: web pages, pictures, slideshows and text. Cameras,
 * captures, media files and colour backgrounds are the show itself. */
bool IsOverlay(obs_source_t *source)
{
	static const char *const kOverlayTypes[] = {"browser_source",   "image_source", "slideshow",
						    "slideshow_v2",     "text_gdiplus", "text_ft2_source",
						    "text_pango_source"};
	const char *id = obs_source_get_unversioned_id(source);
	for (const char *type : kOverlayTypes) {
		if (strcmp(id, type) == 0) {
			return true;
		}
	}
	return IsGazCastOverlay(source);
}

/* Returns a new reference, or nullptr. */
obs_source_t *GetHideFilter(obs_source_t *source)
{
	obs_source_t *found = nullptr;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *out = static_cast<obs_source_t **>(param);
			if (!*out && strcmp(obs_source_get_unversioned_id(filter), kHideFilterId) == 0) {
				*out = obs_source_get_ref(filter);
			}
		},
		&found);
	return found;
}

bool HasHideFilter(obs_source_t *source)
{
	OBSSourceAutoRelease filter = GetHideFilter(source);
	return filter.Get() != nullptr;
}

void SetHidden(obs_source_t *source, bool hidden)
{
	OBSSourceAutoRelease filter = GetHideFilter(source);
	if (hidden && !filter) {
		OBSSourceAutoRelease added =
			obs_source_create(kHideFilterId, obs_module_text("GazCast.Clean.FilterName"), nullptr, nullptr);
		if (added) {
			obs_source_filter_add(source, added);
		}
	} else if (!hidden && filter) {
		obs_source_filter_remove(source, filter);
	}
}

struct SourceRow {
	std::string name;
	bool gazcast;
	bool overlay;
	bool hidden;

	bool operator==(const SourceRow &other) const
	{
		return name == other.name && gazcast == other.gazcast && overlay == other.overlay &&
		       hidden == other.hidden;
	}
};

/* Every input with video, overlays first. Also gives each overlay the filter
 * the first time it is seen; after that the user's tick decides. */
std::vector<SourceRow> ScanSources()
{
	/* Collect first: adding a filter creates a source, which must not happen
	 * inside obs_enum_sources. */
	std::vector<OBSSource> sources;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
				static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
			}
			return true;
		},
		&sources);

	std::vector<SourceRow> rows;
	for (obs_source_t *source : sources) {
		const bool gazcast = IsGazCastOverlay(source);
		const bool overlay = gazcast || IsOverlay(source);
		if (overlay) {
			OBSDataAutoRelease priv = obs_source_get_private_settings(source);
			if (!obs_data_get_bool(priv, kAutoTagged)) {
				SetHidden(source, true);
				obs_data_set_bool(priv, kAutoTagged, true);
				obs_log(LOG_INFO, "clean record: leaving overlay '%s' out",
					obs_source_get_name(source));
			}
		}

		const char *name = obs_source_get_name(source);
		rows.push_back({name ? name : "", gazcast, overlay, HasHideFilter(source)});
	}

	std::stable_sort(rows.begin(), rows.end(), [](const SourceRow &a, const SourceRow &b) {
		if (a.overlay != b.overlay) {
			return a.overlay;
		}
		return QString::fromStdString(a.name).compare(QString::fromStdString(b.name), Qt::CaseInsensitive) < 0;
	});
	return rows;
}

/* ------------------------------------------------------------------------- */
/* Settings: <OBS config>/plugin_config/gazcast-docks/clean-record.json */

bool LoadWithRecording()
{
	BPtr<char> path = obs_module_config_path(kConfigFile);
	OBSDataAutoRelease data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	if (!data) {
		return true;
	}
	obs_data_set_default_bool(data, "with_recording", true);
	return obs_data_get_bool(data, "with_recording");
}

void SaveWithRecording(bool on)
{
	BPtr<char> dir = obs_module_config_path("");
	BPtr<char> path = obs_module_config_path(kConfigFile);
	if (!dir || !path) {
		return;
	}
	os_mkdirs(dir);

	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_bool(data, "with_recording", on);
	if (!obs_data_save_json_safe(data, path, "tmp", "bak")) {
		obs_log(LOG_WARNING, "could not write %s", path.Get());
	}
}

/* ------------------------------------------------------------------------- */
/* Recording */

struct Recorder {
	obs_source_t *mix = nullptr;
	obs_view_t *view = nullptr;
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;
	obs_output_t *output = nullptr;
	bool withMain = false;
	bool stopping = false;
	QString path;
	QElapsedTimer clock;
};

Recorder g_rec;
QString g_lastMessage;
void UpdateDock();

bool EncoderAvailable(const char *id)
{
	const char *type = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
		if (strcmp(type, id) == 0) {
			return true;
		}
	}
	return false;
}

obs_encoder_t *CreateVideoEncoder(obs_output_t *main)
{
	/* Same encoder and settings as the normal recording, when OBS has one
	 * set up. Not a new reference. */
	obs_encoder_t *mainEncoder = main ? obs_output_get_video_encoder(main) : nullptr;
	if (mainEncoder) {
		OBSDataAutoRelease settings = obs_encoder_get_settings(mainEncoder);
		obs_encoder_t *encoder = obs_video_encoder_create(obs_encoder_get_id(mainEncoder),
								  "gazcast-clean-video", settings, nullptr);
		if (encoder) {
			return encoder;
		}
	}

	/* Otherwise the first hardware encoder this PC has, then x264. */
	static const char *const kFallbacks[] = {"obs_nvenc_h264_tex", "jim_nvenc", "h264_texture_amf",
						 "obs_qsv11_v2",       "obs_qsv11", "obs_x264"};
	for (const char *id : kFallbacks) {
		if (!EncoderAvailable(id)) {
			continue;
		}
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "rate_control", "CBR");
		obs_data_set_int(settings, "bitrate", 12000);
		obs_encoder_t *encoder = obs_video_encoder_create(id, "gazcast-clean-video", settings, nullptr);
		if (encoder) {
			return encoder;
		}
	}
	return nullptr;
}

/* The normal recording's first audio track (Advanced output can record other
 * tracks than track 1). Read from the profile so this works on OBS 30. */
size_t RecordingMixer()
{
	config_t *config = obs_frontend_get_profile_config();
	const char *mode = config ? config_get_string(config, "Output", "Mode") : nullptr;
	if (!mode || astrcmpi(mode, "Advanced") != 0) {
		return 0;
	}
	const long long tracks = config_get_int(config, "AdvOut", "RecTracks");
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (tracks & (1LL << i)) {
			return i;
		}
	}
	return 0;
}

obs_encoder_t *CreateAudioEncoder(obs_output_t *main)
{
	const size_t mixer = RecordingMixer();

	obs_encoder_t *mainEncoder = main ? obs_output_get_audio_encoder(main, 0) : nullptr;
	if (mainEncoder) {
		OBSDataAutoRelease settings = obs_encoder_get_settings(mainEncoder);
		obs_encoder_t *encoder = obs_audio_encoder_create(obs_encoder_get_id(mainEncoder),
								  "gazcast-clean-audio", settings, mixer, nullptr);
		if (encoder) {
			return encoder;
		}
	}

	for (const char *id : {"ffmpeg_aac", "CoreAudio_AAC"}) {
		if (!EncoderAvailable(id)) {
			continue;
		}
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "bitrate", 192);
		obs_encoder_t *encoder = obs_audio_encoder_create(id, "gazcast-clean-audio", settings, mixer, nullptr);
		if (encoder) {
			return encoder;
		}
	}
	return nullptr;
}

/* "mp4" when the normal recording uses an MP4 format, otherwise "mkv". */
QString RecordingExtension()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		return QStringLiteral("mkv");
	}
	const char *mode = config_get_string(config, "Output", "Mode");
	const char *section = (mode && astrcmpi(mode, "Advanced") == 0) ? "AdvOut" : "SimpleOutput";
	const char *format = config_get_string(config, section, "RecFormat2");
	return (format && strstr(format, "mp4")) ? QStringLiteral("mp4") : QStringLiteral("mkv");
}

/* Next to the normal recording: "<its name> (clean).<ext>" while it runs,
 * otherwise "GazCast clean <date> <time>.<ext>" in the recording folder. */
QString CleanFilePath(obs_output_t *main, const QString &ext)
{
	if (main && obs_output_active(main)) {
		OBSDataAutoRelease settings = obs_output_get_settings(main);
		QString mainPath = QString::fromUtf8(obs_data_get_string(settings, "path"));
		if (mainPath.isEmpty()) {
			mainPath = QString::fromUtf8(obs_data_get_string(settings, "url"));
		}
		const QFileInfo info(mainPath);
		if (!mainPath.isEmpty() && !mainPath.contains(QLatin1String("://")) && info.dir().exists()) {
			return info.dir().filePath(info.completeBaseName() + QStringLiteral(" (clean).") + ext);
		}
	}

	BPtr<char> folder = obs_frontend_get_current_record_output_path();
	QDir dir((folder && *folder) ? QString::fromUtf8(folder.Get()) : QDir::homePath());
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh-mm-ss"));
	return dir.filePath(QStringLiteral("GazCast clean ") + stamp + QStringLiteral(".") + ext);
}

void OnOutputStop(void *, calldata_t *data);

void ReleaseRecorder()
{
	if (g_rec.output) {
		signal_handler_disconnect(obs_output_get_signal_handler(g_rec.output), "stop", OnOutputStop, nullptr);
		obs_output_release(g_rec.output);
	}
	obs_encoder_release(g_rec.video);
	obs_encoder_release(g_rec.audio);
	if (g_rec.view) {
		obs_view_remove(g_rec.view);
		obs_view_set_source(g_rec.view, 0, nullptr);
		obs_view_destroy(g_rec.view);
	}
	obs_source_release(g_rec.mix);
	g_rec = Recorder();
}

/* Called on the output's thread; finish on the UI thread. */
void OnOutputStop(void *, calldata_t *data)
{
	const long long code = calldata_int(data, "code");
	QString error;
	if (code != OBS_OUTPUT_SUCCESS) {
		obs_output_t *output = static_cast<obs_output_t *>(calldata_ptr(data, "output"));
		const char *last = output ? obs_output_get_last_error(output) : nullptr;
		error = last ? QString::fromUtf8(last) : QString::fromUtf8(obs_module_text("GazCast.Clean.Failed"));
	}

	QMetaObject::invokeMethod(
		QCoreApplication::instance(),
		[error]() {
			if (!g_rec.output) {
				return;
			}
			const QString path = g_rec.path;
			ReleaseRecorder();
			if (error.isEmpty()) {
				g_lastMessage = QString::fromUtf8(obs_module_text("GazCast.Clean.Saved")).arg(path);
				obs_log(LOG_INFO, "clean record: saved %s", path.toUtf8().constData());
			} else {
				g_lastMessage = error;
				obs_log(LOG_WARNING, "clean record stopped with an error: %s",
					error.toUtf8().constData());
			}
			UpdateDock();
		},
		Qt::QueuedConnection);
}

bool StartClean(bool withMain)
{
	if (g_rec.output) {
		return true;
	}

	ScanSources();

	auto fail = [](const QString &why) {
		obs_log(LOG_WARNING, "clean record could not start: %s", why.toUtf8().constData());
		ReleaseRecorder();
		g_lastMessage = why;
		UpdateDock();
		return false;
	};

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}

	g_rec.mix = obs_source_create_private(kMixSourceId, "GazCast clean mix", nullptr);
	g_rec.view = obs_view_create();
	obs_view_set_source(g_rec.view, 0, g_rec.mix);
	video_t *video = obs_view_add2(g_rec.view, &ovi);
	if (!g_rec.mix || !video) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}

	OBSOutputAutoRelease main = obs_frontend_get_recording_output();
	g_rec.video = CreateVideoEncoder(main);
	g_rec.audio = CreateAudioEncoder(main);
	if (!g_rec.video || !g_rec.audio) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.NoEncoder")));
	}
	obs_encoder_set_video(g_rec.video, video);
	obs_encoder_set_audio(g_rec.audio, obs_get_audio());

	const QString ext = RecordingExtension();
	g_rec.path = QDir::toNativeSeparators(CleanFilePath(main, ext));

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "path", g_rec.path.toUtf8().constData());
	if (ext == QLatin1String("mp4")) {
		/* Fragmented, so the file still plays if OBS or the PC crashes. */
		obs_data_set_string(settings, "muxer_settings", "movflags=frag_keyframe+empty_moov+delay_moov");
	}
	g_rec.output = obs_output_create("ffmpeg_muxer", "gazcast-clean-output", settings, nullptr);
	if (!g_rec.output) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}
	obs_output_set_video_encoder(g_rec.output, g_rec.video);
	obs_output_set_audio_encoder(g_rec.output, g_rec.audio, 0);
	signal_handler_connect(obs_output_get_signal_handler(g_rec.output), "stop", OnOutputStop, nullptr);

	if (!obs_output_start(g_rec.output)) {
		const char *last = obs_output_get_last_error(g_rec.output);
		return fail(last ? QString::fromUtf8(last)
				 : QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}

	g_rec.withMain = withMain;
	g_rec.clock.start();
	g_lastMessage.clear();
	obs_log(LOG_INFO, "clean record: started %s (%s encoder)", g_rec.path.toUtf8().constData(),
		obs_encoder_get_id(g_rec.video));
	UpdateDock();
	return true;
}

void StopClean()
{
	if (!g_rec.output || g_rec.stopping) {
		return;
	}
	g_rec.stopping = true;
	obs_output_stop(g_rec.output);
	UpdateDock();
}

/* ------------------------------------------------------------------------- */
/* Dock */

struct DockUi {
	QPointer<QWidget> root;
	QPointer<QLabel> status;
	QPointer<QPushButton> button;
	QPointer<QCheckBox> withRecording;
	QPointer<QListWidget> list;
	QPointer<QTimer> timer;
	std::vector<SourceRow> shown;
	bool added = false;
};

DockUi g_ui;

QString Elapsed()
{
	const qint64 secs = g_rec.clock.isValid() ? g_rec.clock.elapsed() / 1000 : 0;
	return QStringLiteral("%1:%2:%3")
		.arg(secs / 3600, 2, 10, QLatin1Char('0'))
		.arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(secs % 60, 2, 10, QLatin1Char('0'));
}

void UpdateDock()
{
	if (!g_ui.root) {
		return;
	}

	if (g_rec.output) {
		g_ui.status->setText(QString::fromUtf8(obs_module_text(g_rec.stopping ? "GazCast.Clean.Stopping"
										      : "GazCast.Clean.Recording"))
					     .arg(Elapsed(), g_rec.path));
		g_ui.button->setText(QString::fromUtf8(obs_module_text("GazCast.Clean.Stop")));
		g_ui.button->setEnabled(!g_rec.stopping);
	} else {
		g_ui.status->setText(g_lastMessage.isEmpty() ? QString::fromUtf8(obs_module_text("GazCast.Clean.Idle"))
							     : g_lastMessage);
		g_ui.button->setText(QString::fromUtf8(obs_module_text("GazCast.Clean.Start")));
		g_ui.button->setEnabled(true);
	}
}

void RefreshList()
{
	if (!g_ui.list) {
		return;
	}

	std::vector<SourceRow> rows = ScanSources();
	if (rows == g_ui.shown) {
		return;
	}
	g_ui.shown = rows;

	const QSignalBlocker block(g_ui.list);
	const int scroll = g_ui.list->verticalScrollBar() ? g_ui.list->verticalScrollBar()->value() : 0;
	g_ui.list->clear();
	for (const SourceRow &row : rows) {
		QString label = QString::fromStdString(row.name);
		if (row.overlay) {
			label += QStringLiteral("  ") +
				 QString::fromUtf8(obs_module_text(row.gazcast ? "GazCast.Clean.GazCastTag"
									       : "GazCast.Clean.OverlayTag"));
		}
		auto *item = new QListWidgetItem(label, g_ui.list);
		item->setData(Qt::UserRole, QString::fromStdString(row.name));
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(row.hidden ? Qt::Checked : Qt::Unchecked);
	}
	if (g_ui.list->verticalScrollBar()) {
		g_ui.list->verticalScrollBar()->setValue(scroll);
	}
}

void OnItemChanged(QListWidgetItem *item)
{
	const std::string name = item->data(Qt::UserRole).toString().toStdString();
	OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
	if (source) {
		SetHidden(source, item->checkState() == Qt::Checked);
	}
	RefreshList();
}

QWidget *BuildDock()
{
	auto *root = new QWidget();
	root->setObjectName(QStringLiteral("gazcast-clean-container"));
	root->setMinimumSize(220, 200);

	auto *layout = new QVBoxLayout(root);
	layout->setContentsMargins(8, 8, 8, 8);

	auto *status = new QLabel(root);
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(status);

	auto *button = new QPushButton(root);
	button->setMinimumHeight(32);
	layout->addWidget(button);

	auto *withRecording = new QCheckBox(QString::fromUtf8(obs_module_text("GazCast.Clean.WithRecording")), root);
	withRecording->setChecked(LoadWithRecording());
	layout->addWidget(withRecording);

	auto *hint = new QLabel(QString::fromUtf8(obs_module_text("GazCast.Clean.LeaveOut")), root);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	auto *list = new QListWidget(root);
	list->setSelectionMode(QAbstractItemView::NoSelection);
	layout->addWidget(list, 1);

	QObject::connect(button, &QPushButton::clicked, root, []() {
		if (g_rec.output) {
			StopClean();
		} else {
			StartClean(false);
		}
	});
	QObject::connect(withRecording, &QCheckBox::toggled, root, [](bool on) { SaveWithRecording(on); });
	QObject::connect(list, &QListWidget::itemChanged, root, OnItemChanged);

	/* Keeps the list (and the automatic tagging of new GazCast overlays) up
	 * to date, and ticks the recording clock. */
	auto *timer = new QTimer(root);
	timer->setInterval(1000);
	int tick = 0;
	QObject::connect(timer, &QTimer::timeout, root, [tick]() mutable {
		if (g_rec.output) {
			UpdateDock();
		}
		if (++tick % 2 == 0) {
			RefreshList();
		}
	});
	timer->start();

	g_ui.root = root;
	g_ui.status = status;
	g_ui.button = button;
	g_ui.withRecording = withRecording;
	g_ui.list = list;
	g_ui.timer = timer;
	return root;
}

} // namespace

/* ------------------------------------------------------------------------- */

void CleanRecordRegisterTypes()
{
	obs_source_info mix = {};
	mix.id = kMixSourceId;
	mix.type = OBS_SOURCE_TYPE_INPUT;
	mix.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	mix.get_name = MixGetName;
	mix.create = MixCreate;
	mix.destroy = MixDestroy;
	mix.get_width = MixGetWidth;
	mix.get_height = MixGetHeight;
	mix.video_render = MixRender;
	obs_register_source(&mix);

	obs_source_info hide = {};
	hide.id = kHideFilterId;
	hide.type = OBS_SOURCE_TYPE_FILTER;
	hide.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	hide.get_name = HideGetName;
	hide.create = HideCreate;
	hide.destroy = HideDestroy;
	hide.video_render = HideRender;
	obs_register_source(&hide);
}

void CleanRecordCreateDock()
{
	QWidget *root = BuildDock();
	g_ui.added = obs_frontend_add_dock_by_id(kDockId, obs_module_text("GazCast.Dock.Clean"), root);
	if (!g_ui.added) {
		obs_log(LOG_WARNING, "dock id '%s' already in use", kDockId);
		delete root;
		g_ui = DockUi();
		return;
	}
	UpdateDock();
}

void CleanRecordFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (!g_rec.output && LoadWithRecording()) {
			StartClean(true);
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (g_rec.output && g_rec.withMain) {
			StopClean();
		}
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		g_ui.shown.clear();
		RefreshList();
		break;
	default:
		break;
	}
}

void CleanRecordShutdown()
{
	if (g_ui.timer) {
		g_ui.timer->stop();
	}
	if (g_rec.output) {
		/* OBS is closing: stop now instead of waiting for the muxer. */
		signal_handler_disconnect(obs_output_get_signal_handler(g_rec.output), "stop", OnOutputStop, nullptr);
		obs_output_force_stop(g_rec.output);
	}
	ReleaseRecorder();

	if (g_ui.added) {
		obs_frontend_remove_dock(kDockId);
	}
	g_ui = DockUi();
}
