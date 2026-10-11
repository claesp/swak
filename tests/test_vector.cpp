#include "core/gdal_support.h"
#include "core/maps.h"
#include "core/tiles.h"
#include "core/vector_tiles.h"
#include "image_fault.h"
#include "tile_server.h"
#include "vector_fixture.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QPainter>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <thread>
Q_IMPORT_PLUGIN(MvtProvider)
using namespace swak;
class SlowVector : public MapProvider, public VectorTileProvider
{
public:
    MapProvider &base;
    mutable std::atomic_int started{0};
    mutable std::atomic_int finished{0};
    mutable std::atomic_int limitFailures{0};
    bool throws = false;
    bool dense = false;
    VectorTileProvider *inflated = nullptr;
    explicit SlowVector(MapProvider &provider) : base(provider) {}
    QString mapId() const override
    {
        return base.mapId();
    }
    QString mapName() const override
    {
        return base.mapName();
    }
    QJsonObject defaults() const override
    {
        return base.defaults();
    }
    QList<SettingField> fields() const override
    {
        return base.fields();
    }
    QString validate(const QJsonObject &source) const override
    {
        return base.validate(source);
    }
    QUrl tileUrl(const QJsonObject &source, int z, int x, int y) const override
    {
        return base.tileUrl(source, z, x, y);
    }
    QStringList layerNames() const override
    {
        return {};
    }
    VectorTile decodeTile(const QByteArray &bytes, const std::atomic_bool &cancelled) const override
    {
        ++started;
        if (limitFailures.fetch_sub(1) > 0) {
            VectorTile tile;
            tile.resourceLimit = true;
            tile.error = QStringLiteral("Fixture resource limit");
            ++finished;
            return tile;
        }
        if (inflated) {
            auto result = inflated->decodeTile(bytes, cancelled);
            if (dense && !result.features.isEmpty()) {
                auto &path = result.features.first().path;
                for (int i = 0; i < 60000 && !cancelled.load(); ++i) {
                    path.lineTo(0.02 + i * 0.000001, 0.02);
                }
                path.closeSubpath();
            }
            result.costKiB = 40000;
            ++finished;
            return result;
        }
        if (throws) {
            ++finished;
            throw std::runtime_error("test decoder failure");
        }
        for (int i = 0; i < 2000 && !cancelled.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++finished;
        return {};
    }
};
class VectorTest final : public QObject
{
    Q_OBJECT
    MapProvider *provider_ = nullptr;
    VectorTileProvider *decoder_ = nullptr;
    std::atomic_bool cancelled_{false};
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        for (auto *object : QPluginLoader::staticInstances()) {
            if (auto *provider = qobject_cast<MapProvider *>(object)) {
                provider_ = provider;
            }
            if (auto *decoder = qobject_cast<VectorTileProvider *>(object)) {
                decoder_ = decoder;
            }
        }
        QVERIFY(provider_);
        QVERIFY(decoder_);
    }
    void decodeAndInspect()
    {
        const auto tile = decoder_->decodeTile(vector_fixture::sample(), cancelled_);
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QCOMPARE(tile.features.size(), 3);
        QCOMPARE(tile.layers.size(), 3);
        QCOMPARE(tile.features.last().id, QStringLiteral("9007199254740993"));
        QCOMPARE(tile.features.last().points.first(), QPointF(0.5, 0.5));
        QCOMPARE(tile.features.last().properties.value(QStringLiteral("class")).toString(),
                 QStringLiteral("cafe"));
        const auto hits = vectorFeaturesAt(
            tile, QRectF(0, 0, 256, 256), QPointF(128, 128), provider_->defaults());
        QCOMPARE(hits.size(), 3);
        QCOMPARE(hits.first().value(QStringLiteral("layer")).toString(), QStringLiteral("poi"));
        auto source = provider_->defaults();
        source.insert(QStringLiteral("visibleLayers"), QJsonObject{{QStringLiteral("poi"), false}});
        QCOMPARE(vectorFeaturesAt(tile, QRectF(0, 0, 256, 256), QPointF(128, 128), source).size(),
                 2);
        QVERIFY(vectorFeaturesAt(tile, QRectF(0, 0, 256, 256), QPointF(-1, -1), source).isEmpty());
        QImage canvas(256, 256, QImage::Format_RGB32);
        QPainter painter(&canvas);
        QList<QRectF> labels;
        paintVectorTile(painter, tile, QRectF(0, 0, 256, 256), source, labels);
        QCOMPARE(canvas.pixelColor(50, 50), QColor("#92bfd0"));
        QVERIFY(!labels.isEmpty());
    }
    void labelTargets()
    {
        VectorTile tile;
        VectorFeature feature;
        feature.kind = VectorFeature::Kind::Point;
        feature.layer = QStringLiteral("poi");
        feature.points = {{0.25, 0.5}};
        feature.properties = {{QStringLiteral("name"), QStringLiteral("A selectable label")}};
        tile.features.append(feature);
        auto source = provider_->defaults();
        const auto labels = vectorLabels(tile, source);
        QCOMPARE(labels.size(), 1);
        const auto point = labels.first().bounds.center();
        QVERIFY(QLineF(QPointF(64, 128), point).length() > 14);
        QCOMPARE(vectorFeaturesAt(tile, QRectF(0, 0, 256, 256), point, source).size(), 1);
        source.insert(QStringLiteral("labels"), false);
        QVERIFY(vectorLabels(tile, source).isEmpty());
        QVERIFY(vectorFeaturesAt(tile, QRectF(0, 0, 256, 256), point, source).isEmpty());
        source.insert(QStringLiteral("labels"), true);
        tile.features.append(feature);
        QCOMPARE(vectorLabels(tile, source).size(), 1);
        std::atomic_bool cancelled{true};
        QVERIFY(vectorLabels(tile, source, &cancelled).isEmpty());
    }
    void denseTileLimits()
    {
        // Spread the points over many features. One huge multipoint makes the GDAL decoder grow
        // its point list one allocation at a time, which is slow under the address sanitizer.
        const auto tile = [](int features, int points) {
            using namespace vector_fixture;
            QByteArray geometry = varint((static_cast<quint64>(points) << 3U) | 1U);
            geometry.append(QByteArray(points * 2, '\0'));
            const auto one = field(2, feature(1, geometry));
            QByteArray body = field(1, "poi");
            for (int i = 0; i < features; ++i) {
                body += one;
            }
            body += field(3, "name") + field(3, "class") + field(4, field(1, "Test place")) +
                    field(4, field(1, "cafe")) + number(5, 4096) + number(15, 2);
            return field(3, body);
        };
        const auto dense = decoder_->decodeTile(tile(100, 1001), cancelled_);
        QVERIFY2(dense.error.isEmpty(), qPrintable(dense.error));
        QCOMPARE(dense.features.size(), 100);
        qsizetype points = 0;
        for (const auto &feature : dense.features) {
            points += feature.points.size();
        }
        QCOMPARE(points, 100100);
        const auto excessive = decoder_->decodeTile(tile(600, 1001), cancelled_);
        QVERIFY(!excessive.error.isEmpty());
        QVERIFY(excessive.resourceLimit);
        const auto oversized =
            decoder_->decodeTile(QByteArray(VectorDownloadLimit + 1, 'x'), cancelled_);
        QVERIFY(oversized.resourceLimit);
    }
    void invalidInput()
    {
        const auto good = vector_fixture::sample();
        QVERIFY(!decoder_->decodeTile(QByteArray(2 * 1024 * 1024 + 1, 'x'), cancelled_)
                     .error.isEmpty());
        QVERIFY(
            !decoder_->decodeTile(QByteArray::fromHex("1affffffff7f"), cancelled_).error.isEmpty());
        QVERIFY(
            !decoder_
                 ->decodeTile(vector_fixture::layer(
                                  "poi", vector_fixture::feature(1, vector_fixture::geometry({0}))),
                              cancelled_)
                 .error.isEmpty());
        const auto truncated = decoder_->decodeTile(
            vector_fixture::layer("poi",
                                  vector_fixture::feature(1, vector_fixture::geometry({9, 1}))),
            cancelled_);
        // GDAL drops a feature with a truncated geometry command.
        QVERIFY(truncated.features.isEmpty());
        QVERIFY(
            !decoder_
                 ->decodeTile(
                     vector_fixture::layer(
                         "poi", vector_fixture::feature(1, vector_fixture::geometry({9, 1, 1})), 3),
                     cancelled_)
                 .error.isEmpty());
        QVERIFY(!decoder_->decodeTile(good + good, cancelled_).error.isEmpty());
        cancelled_.store(true);
        QVERIFY(!decoder_->decodeTile(good, cancelled_).error.isEmpty());
        cancelled_.store(false);
        for (qsizetype i = 0; i < good.size(); ++i) {
            auto bytes = good;
            bytes[i] = static_cast<char>(0xff);
            decoder_->decodeTile(bytes, cancelled_);
            decoder_->decodeTile(good.left(i), cancelled_);
        }
    }
    void preferences()
    {
        const QString id = provider_->mapId();
        MapSources sources(QPluginLoader::staticInstances());
        auto legacy = sources.savePreferences();
        legacy.remove(QStringLiteral("knownProviders"));
        legacy.insert(QStringLiteral("sources"), QJsonObject{});
        sources.restorePreferences(legacy);
        QVERIFY(sources.sources().contains(id));
        sources.remove(id);
        const auto saved = sources.savePreferences();
        MapSources restored(QPluginLoader::staticInstances());
        restored.restorePreferences(saved);
        QVERIFY(!restored.sources().contains(id));
        auto source = provider_->defaults();
        source.insert(QStringLiteral("format"), id);
        source.insert(QStringLiteral("visibleLayers"), QJsonObject{{QStringLiteral("poi"), false}});
        QVERIFY(sources.put(id, source).isEmpty());
        sources.select(id);
        restored.restorePreferences(sources.savePreferences());
        QVERIFY(!vectorLayerVisible(restored.activeSource(), QStringLiteral("poi")));
        QCOMPARE(restored.activeSource().value(QStringLiteral("maxZoom")).toInt(), 14);
        source.insert(QStringLiteral("labelSize"), 100);
        QVERIFY(!provider_->validate(source).isEmpty());
        source = provider_->defaults();
        source.insert(QStringLiteral("language"), QStringLiteral("../bad"));
        QVERIFY(!provider_->validate(source).isEmpty());
    }
    void cancellationAndExceptions()
    {
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto source = provider_->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/slow/{z}/{x}/{y}.pbf").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        SlowVector slow(*provider_);
        QImage canvas(512, 512, QImage::Format_RGB32);
        const auto draw = [&](TileRenderer &renderer) {
            QPainter painter(&canvas);
            renderer.paint(painter, {QSizeF(512, 512), QPointF(0.5, 0.5), 1}, 1, false);
        };
        {
            TileRenderer renderer;
            renderer.setPrefetchMargin(0);
            renderer.setSource(&slow, source);
            draw(renderer);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return slow.started.load() >= 2;
                },
                5000));
            {
                QPainter painter(&canvas);
                renderer.paint(painter, {QSizeF(512, 512), QPointF(0, 0), 1}, 1, false);
            }
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return slow.finished.load() >= 1;
                },
                1000));
            renderer.setSource(provider_, source);
            draw(renderer);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return !renderer.featuresAt(QPointF(128, 128)).isEmpty();
                },
                5000));
            QVERIFY(slow.finished.load() >= 2);
            renderer.setSource(&slow, source);
            draw(renderer);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return slow.started.load() > slow.finished.load();
                },
                5000));
        }
        QCOMPARE(slow.started.load(), slow.finished.load());
        slow.throws = true;
        TileRenderer renderer;
        renderer.setPrefetchMargin(0);
        renderer.setSource(&slow, source);
        draw(renderer);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty() &&
                       !renderer.status().startsWith(QStringLiteral("Map coverage:"));
            },
            5000));
        QVERIFY(renderer.featuresAt(QPointF(128, 128)).isEmpty());
    }
    void imageAllocationFailure()
    {
        // A vector tile whose image cannot be made fails with a status. The next source renders.
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto source = provider_->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/{z}/{x}/{y}.pbf").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        QImage canvas(512, 512, QImage::Format_RGB32);
        TileRenderer renderer;
        renderer.setPrefetchMargin(0);
        const auto draw = [&] {
            QPainter painter(&canvas);
            renderer.paint(painter, {QSizeF(512, 512), QPointF(0.5, 0.5), 1}, 1, false);
        };
        const auto failsWith = [&](swak::ImageFault fault, const QString &text) {
            const ImageFaultGuard guard(fault);
            renderer.setSource(provider_, source);
            return QTest::qWaitFor(
                [&] {
                    draw();
                    return renderer.status().contains(text);
                },
                10000);
        };
        QVERIFY2(failsWith(swak::ImageFault::NullImage,
                           QStringLiteral("Vector tile image allocation failed.")),
                 qPrintable(renderer.status()));
        QCOMPARE(renderer.coverage().rendered, 0);
        QVERIFY2(
            failsWith(swak::ImageFault::Throw, QStringLiteral("Vector tile preparation failed.")),
            qPrintable(renderer.status()));
        QCOMPARE(renderer.coverage().rendered, 0);
        renderer.setSource(provider_, source);
        QVERIFY(QTest::qWaitFor(
            [&] {
                draw();
                return !renderer.featuresAt(QPointF(128, 128)).isEmpty();
            },
            10000));
    }
    void cachePressure()
    {
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto source = provider_->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/large/{z}/{x}/{y}.pbf").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        SlowVector large(*provider_);
        large.inflated = decoder_;
        TileRenderer renderer;
        renderer.setSource(&large, source);
        QImage canvas(512, 512, QImage::Format_RGB32);
        Viewport view{QSizeF(512, 512), QPointF(0.5, 0.5), 1};
        const auto draw = [&] {
            QPainter painter(&canvas);
            renderer.paint(painter, view, 1, false);
        };
        connect(&renderer, &TileRenderer::changed, &renderer, draw);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.memoryCostKiB() == 164096;
            },
            5000));
        QVERIFY(renderer.status().isEmpty());
        QCOMPARE(renderer.memoryCostKiB(), 164096);
        for (int y : {128, 384}) {
            for (int x : {128, 384}) {
                QVERIFY(!renderer.featuresAt(QPointF(x, y)).isEmpty());
            }
        }
        renderer.setMemoryLimit(64);
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto state = renderer.coverage();
                return state.total == 4 && state.rendered == 4;
            },
            5000));
        QCOMPARE(renderer.memoryCostKiB(), 1024);
        QVERIFY(renderer.status().contains(QStringLiteral("Map complete")));
        for (int y : {64, 320}) {
            for (int x : {64, 320}) {
                QCOMPARE(canvas.pixelColor(x, y), QColor("#92bfd0"));
            }
        }
        const int decoded = large.started.load();
        draw();
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return large.started.load() > decoded;
            },
            500));
        QVERIFY(std::none_of(
            server.requests.begin(), server.requests.end(), [](const QByteArray &request) {
                return request.contains("/large/0/0/0.pbf");
            }));
        source.insert(QStringLiteral("minZoom"), 1);
        renderer.setSource(&large, source);
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto state = renderer.coverage();
                return state.total == 4 && state.rendered == 4;
            },
            5000));
        QCOMPARE(renderer.memoryCostKiB(), 1024);
        for (const double center : {0.375, 0.625, 0.375}) {
            view.zoom = 2;
            view.center = QPointF(center, center);
            draw();
            QVERIFY(QTest::qWaitFor(
                [&] {
                    const auto state = renderer.coverage();
                    return state.total == 9 && state.rendered == 9;
                },
                5000));
            QVERIFY(renderer.memoryCostKiB() <= 64 * 1024);
            for (int y : {64, 320}) {
                for (int x : {64, 320}) {
                    QCOMPARE(canvas.pixelColor(x, y), QColor("#92bfd0"));
                }
            }
        }
        view.zoom = 1;
        view.center = QPointF(0.5, 0.5);
        renderer.setMemoryLimit(256);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.memoryCostKiB() == 164096;
            },
            5000));
        QVERIFY(renderer.status().isEmpty());
        renderer.stop();
    }
    void coverageRecovery()
    {
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto source = provider_->defaults();
        source.insert(QStringLiteral("endpoint"),
                      QStringLiteral("https://localhost:%1/recovery/{z}/{x}/{y}.pbf")
                          .arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        SlowVector limited(*provider_);
        limited.inflated = decoder_;
        limited.limitFailures = 1;
        TileRenderer renderer;
        renderer.setPrefetchMargin(0);
        renderer.setSource(&limited, source);
        QImage canvas(512, 512, QImage::Format_RGB32);
        const auto draw = [&] {
            QPainter painter(&canvas);
            renderer.paint(painter, {QSizeF(512, 512), QPointF(0.5, 0.5), 1}, 1, false);
        };
        connect(&renderer, &TileRenderer::changed, &renderer, draw);
        draw();
        QCOMPARE(renderer.coverage().rendered, 0);
        QCOMPARE(renderer.coverage().total, 4);
        QVERIFY(renderer.status().contains(QStringLiteral("0/4")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto state = renderer.coverage();
                return state.total == 1 && state.rendered == 1;
            },
            5000));
        QVERIFY(std::any_of(
            server.requests.begin(), server.requests.end(), [](const QByteArray &request) {
                return request.contains("/recovery/0/0/0.pbf");
            }));
        QVERIFY(renderer.status().isEmpty());
        renderer.stop();
        QCOMPARE(renderer.coverage().total, 0);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.coverage().rendered == renderer.coverage().total &&
                       renderer.coverage().total > 0;
            },
            5000));
        renderer.stop();
        const auto before = server.requests.size();
        source.insert(QStringLiteral("minZoom"), 1);
        source.insert(QStringLiteral("endpoint"),
                      QStringLiteral("https://localhost:%1/minimum/{z}/{x}/{y}.pbf")
                          .arg(server.serverPort()));
        limited.limitFailures = 100;
        renderer.setSource(&limited, source);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return server.requests.size() == before + 4 && renderer.pendingCount() == 0 &&
                       limited.started == limited.finished;
            },
            5000));
        QCOMPARE(renderer.coverage().total, 4);
        QCOMPARE(renderer.coverage().rendered, 0);
        QVERIFY(renderer.status().contains(QStringLiteral("Fixture resource limit")));
        const auto failed = server.requests.size();
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return server.requests.size() > failed;
            },
            2200));
        renderer.stop();
        source.insert(QStringLiteral("minZoom"), 7);
        source.insert(QStringLiteral("maxZoom"), 7);
        disconnect(&renderer, &TileRenderer::changed, &renderer, nullptr);
        renderer.setSource(&limited, source);
        {
            QPainter painter(&canvas);
            renderer.paint(painter, {QSizeF(32768, 32768), QPointF(0.5, 0.5), 7}, 1, false);
        }
        QCOMPARE(renderer.coverage().total, 16384);
        QCOMPARE(renderer.coverage().rendered, 0);
        renderer.stop();
    }
    void newViewportBypassesOldRetry()
    {
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto source = provider_->defaults();
        source.insert(QStringLiteral("minZoom"), 1);
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/retry/{z}/{x}/{y}.pbf").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        SlowVector limited(*provider_);
        limited.inflated = decoder_;
        limited.limitFailures = 1;
        TileRenderer renderer;
        renderer.setPrefetchMargin(0);
        renderer.setSource(&limited, source);
        QImage canvas(256, 256, QImage::Format_RGB32);
        Viewport view{QSizeF(256, 256), QPointF(0.25, 0.25), 1};
        const auto draw = [&] {
            QPainter painter(&canvas);
            renderer.paint(painter, view, 1, false);
        };
        connect(&renderer, &TileRenderer::changed, &renderer, draw);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.status().contains(QStringLiteral("Fixture resource limit"));
            },
            5000));
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return server.requests.size() > 1;
            },
            300));
        view.center = QPointF(0.75, 0.25);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.coverage().rendered == 1;
            },
            3000));
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(renderer.status().isEmpty());
        renderer.stop();
    }
    void nearbyCachePressure()
    {
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto source = provider_->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/nearby/{z}/{x}/{y}.pbf").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        SlowVector large(*provider_);
        large.inflated = decoder_;
        TileRenderer renderer;
        renderer.setMemoryLimit(64);
        renderer.setSource(&large, source);
        QImage canvas(256, 256, QImage::Format_RGB32);
        const auto draw = [&] {
            QPainter painter(&canvas);
            renderer.paint(painter, {QSizeF(256, 256), QPointF(0.375, 0.375), 2}, 1, false);
        };
        connect(&renderer, &TileRenderer::changed, &renderer, draw);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return large.finished.load() == 9;
            },
            5000));
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return large.started.load() > 9;
            },
            300));
        QCOMPARE(renderer.memoryCostKiB(), 41024);
        QVERIFY(!renderer.featuresAt(QPointF(128, 128)).isEmpty());
        QVERIFY(renderer.status().isEmpty());
        QVERIFY(std::none_of(
            server.requests.begin(), server.requests.end(), [](const QByteArray &request) {
                return request.contains("/nearby/1/");
            }));
        renderer.stop();
    }
    void densePanning()
    {
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto source = provider_->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/dense/{z}/{x}/{y}.pbf").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        SlowVector dense(*provider_);
        dense.inflated = decoder_;
        dense.dense = true;
        TileRenderer renderer;
        renderer.setSource(&dense, source);
        QImage canvas(512, 512, QImage::Format_RGB32);
        const auto draw = [&](double offset) {
            QPainter painter(&canvas);
            renderer.paint(painter, {QSizeF(512, 512), QPointF(0.5 + offset, 0.5), 1}, 1, false);
        };
        draw(0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.memoryCostKiB() == 164096;
            },
            5000));
        draw(0);
        // Every tile must retain its geometry after the old frame budget is exceeded.
        for (int y : {50, 306}) {
            for (int x : {50, 306}) {
                QCOMPARE(canvas.pixelColor(x, y), QColor("#92bfd0"));
            }
        }
        const int decoded = dense.started.load();
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < 120; ++i) {
            draw((i % 20) / 10000.0);
        }
        qInfo() << "Dense cached pan, 120 frames, milliseconds:" << clock.elapsed();
        QCOMPARE(dense.started.load(), decoded);
        QCOMPARE(renderer.memoryCostKiB(), 164096);
        QVERIFY(renderer.status().isEmpty());
        source.insert(QStringLiteral("visibleLayers"),
                      QJsonObject{{QStringLiteral("water"), false}});
        renderer.setSource(&dense, source);
        draw(0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.memoryCostKiB() == 164096;
            },
            5000));
        draw(0);
        for (int y : {50, 306}) {
            for (int x : {50, 306}) {
                QCOMPARE(canvas.pixelColor(x, y), QColor("#e9e6dc"));
            }
        }
        renderer.stop();
    }
    void holesAndProperties()
    {
        using namespace vector_fixture;
        auto bytes = layer(
            "landuse", feature(3, geometry({9,    0,    0,  26, 8192, 0,    0, 8192, 8191, 0, 15, 9,
                                            2048, 6143, 26, 0,  4096, 4096, 0, 0,    4095, 15})));
        const auto tile = decoder_->decodeTile(bytes, cancelled_);
        QVERIFY(tile.error.isEmpty());
        QVERIFY(!tile.features.first().path.contains(QPointF(0.5, 0.5)));
        QVERIFY(tile.features.first().path.contains(QPointF(0.1, 0.1)));
        const auto point = feature(1, geometry({9, 4096, 4096}));
        for (const auto &value : {number(4, 123),
                                  number(5, 123),
                                  number(6, 1),
                                  number(7, 1),
                                  QByteArray::fromHex("150000803f"),
                                  QByteArray::fromHex("19000000000000f03f")}) {
            const auto message =
                field(3,
                      field(1, "poi") + field(2, point) + field(3, "name") + field(3, "value") +
                          field(4, field(1, "Test")) + field(4, value + number(20, 1)) +
                          number(15, 2) + number(20, 1));
            const auto decoded = decoder_->decodeTile(message, cancelled_);
            QVERIFY(decoded.error.isEmpty());
            QVERIFY(decoded.features.first().properties.contains(QStringLiteral("value")));
        }
        const auto bad = field(3, field(1, "poi") + field(2, point) + number(15, 4294967298ULL));
        const auto badVersion = decoder_->decodeTile(bad, cancelled_);
        // GDAL drops a layer with an unsupported version.
        QVERIFY(badVersion.features.isEmpty());
        QImage canvas(256, 256, QImage::Format_RGB32);
        QPainter painter(&canvas);
        QList<QRectF> labels;
        paintVectorTile(painter, tile, QRectF(0, 0, 256, 256), provider_->defaults(), labels);
        QCOMPARE(canvas.pixelColor(128, 128), QColor("#e9e6dc"));
        QCOMPARE(canvas.pixelColor(25, 25), tile.features.first().color);
        cancelled_.store(true);
        paintVectorTile(
            painter, tile, QRectF(0, 0, 256, 256), provider_->defaults(), labels, &cancelled_);
        QCOMPARE(canvas.pixelColor(25, 25), QColor("#e9e6dc"));
        cancelled_.store(false);
    }
    void networkAndReplacement()
    {
        TileServer server;
        server.body =
            vector_fixture::sample() + vector_fixture::field(99, QByteArray(3 * 1024 * 1024, 'x'));
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto source = provider_->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/{z}/{x}/{y}.pbf").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        TileRenderer renderer;
        QImage canvas(256, 256, QImage::Format_RGB32);
        const auto draw = [&] {
            QPainter painter(&canvas);
            renderer.paint(painter, {QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 1, false);
        };
        renderer.setSource(provider_, source);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.featuresAt(QPointF(128, 128)).isEmpty();
            },
            5000));
        draw();
        QCOMPARE(canvas.pixelColor(50, 50), QColor("#92bfd0"));
        QCOMPARE(renderer.itemsAt(QPointF(128, 128)).size(),
                 renderer.featuresAt(QPointF(128, 128)).size());
        QVERIFY(renderer.itemsAt(QPointF(-1, -1)).isEmpty());
        QVERIFY(renderer.layers().contains(QStringLiteral("poi")));
        source.insert(QStringLiteral("visibleLayers"), QJsonObject{{QStringLiteral("poi"), false}});
        renderer.setSource(provider_, source);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.featuresAt(QPointF(128, 128)).size() == 2;
            },
            5000));
        renderer.setSource(nullptr, {});
        QVERIFY(renderer.featuresAt(QPointF(128, 128)).isEmpty());
        source.insert(QStringLiteral("endpoint"),
                      QStringLiteral("https://localhost:%1/invalid/{z}/{x}/{y}.pbf")
                          .arg(server.serverPort()));
        server.body = "bad tile";
        renderer.setSource(provider_, source);
        draw();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty() &&
                       !renderer.status().startsWith(QStringLiteral("Map coverage:"));
            },
            5000));
        QVERIFY(renderer.featuresAt(QPointF(128, 128)).isEmpty());
        renderer.stop();
        QVERIFY(renderer.featuresAt(QPointF(128, 128)).isEmpty());
    }
};
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Close cached GDAL resources after the tests and before the leak check.
    swak::GdalShutdownGuard gdal;
    if (argc == 3 &&
        (QByteArray(argv[1]) == "--sample" || QByteArray(argv[1]) == "--render-sample" ||
         QByteArray(argv[1]) == "--benchmark-sample")) {
        QFile file(QString::fromLocal8Bit(argv[2]));
        if (!file.open(QIODevice::ReadOnly)) {
            return 2;
        }
        const auto bytes = file.read(VectorDownloadLimit + 1);
        std::atomic_bool cancelled{false};
        for (auto *object : QPluginLoader::staticInstances()) {
            if (auto *decoder = qobject_cast<VectorTileProvider *>(object)) {
                const auto tile = decoder->decodeTile(bytes, cancelled);
                qInfo().noquote() << "Features:" << tile.features.size()
                                  << "Cost KiB:" << tile.costKiB
                                  << "Layers:" << tile.layers.join(',') << "Error:" << tile.error;
                if (tile.error.isEmpty() && QByteArray(argv[1]) == "--render-sample") {
                    QImage image(1024, 1024, QImage::Format_RGB32);
                    {
                        QPainter painter(&image);
                        painter.setRenderHint(QPainter::Antialiasing);
                        QList<QRectF> labels;
                        paintVectorTile(painter,
                                        tile,
                                        QRectF(0, 0, 1024, 1024),
                                        qobject_cast<MapProvider *>(object)->defaults(),
                                        labels);
                    }
                    if (!image.save(QStringLiteral("vector-sample.png"))) {
                        return 2;
                    }
                }
                if (tile.error.isEmpty() && QByteArray(argv[1]) == "--benchmark-sample") {
                    const auto source = qobject_cast<MapProvider *>(object)->defaults();
                    QImage canvas(1024, 1024, QImage::Format_RGB32);
                    QImage cached(512, 512, QImage::Format_ARGB32_Premultiplied);
                    {
                        QPainter painter(&cached);
                        painter.scale(2, 2);
                        painter.setRenderHint(QPainter::Antialiasing);
                        QList<QRectF> labels;
                        paintVectorTile(painter, tile, QRectF(0, 0, 256, 256), source, labels);
                    }
                    for (bool reuse : {false, true}) {
                        QElapsedTimer timer;
                        timer.start();
                        for (int frame = 0; frame < 10; ++frame) {
                            QPainter painter(&canvas);
                            painter.setRenderHint(QPainter::Antialiasing);
                            QList<QRectF> labels;
                            for (int y = 0; y < 4; ++y) {
                                for (int x = 0; x < 4; ++x) {
                                    const QRectF rect(x * 256 - frame, y * 256, 256, 256);
                                    if (reuse) {
                                        painter.drawImage(rect, cached);
                                    } else {
                                        paintVectorTile(painter, tile, rect, source, labels);
                                    }
                                }
                            }
                        }
                        qInfo() << (reuse ? "Cached" : "Direct")
                                << "16 tiles, 10 pan frames, milliseconds:" << timer.elapsed();
                    }
                }
                return tile.error.isEmpty() ? 0 : 1;
            }
        }
        return 2;
    }
    VectorTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_vector.moc"
