/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SlideDir.h"

#include "SkAnimTimer.h"
#include "SkCanvas.h"
#include "SkMakeUnique.h"
#include "SkSGColor.h"
#include "SkSGDraw.h"
#include "SkSGGroup.h"
#include "SkSGRect.h"
#include "SkSGRenderNode.h"
#include "SkSGScene.h"
#include "SkSGText.h"
#include "SkSGTransform.h"
#include "SkTypeface.h"

#include <cmath>

namespace {

static constexpr float  kAspectRatio   = 1.5f;
static constexpr float  kLabelSize     = 12.0f;
static constexpr SkSize kPadding       = { 12.0f , 24.0f };
static constexpr SkSize kFocusInset    = { 100.0f, 100.0f };
static constexpr float  kFocusDuration = 250;

// TODO: better unfocus binding?
static constexpr SkUnichar kUnfocusKey = ' ';

class SlideAdapter final : public sksg::RenderNode {
public:
    explicit SlideAdapter(sk_sp<Slide> slide)
        : fSlide(std::move(slide)) {
        SkASSERT(fSlide);
    }

    std::unique_ptr<sksg::Animator> makeForwardingAnimator() {
        // Trivial sksg::Animator -> skottie::Animation tick adapter
        class ForwardingAnimator final : public sksg::Animator {
        public:
            explicit ForwardingAnimator(sk_sp<SlideAdapter> adapter)
                : fAdapter(std::move(adapter)) {}

        protected:
            void onTick(float t) override {
                fAdapter->tick(SkScalarRoundToInt(t));
            }

        private:
            sk_sp<SlideAdapter> fAdapter;
        };

        return skstd::make_unique<ForwardingAnimator>(sk_ref_sp(this));
    }

protected:
    SkRect onRevalidate(sksg::InvalidationController* ic, const SkMatrix& ctm) override {
        const auto isize = fSlide->getDimensions();
        return SkRect::MakeIWH(isize.width(), isize.height());
    }

    void onRender(SkCanvas* canvas) const override {
        fSlide->draw(canvas);
    }

private:
    void tick(SkMSec t) {
        fSlide->animate(SkAnimTimer(0, t * 1e6, SkAnimTimer::kRunning_State));
        this->invalidate();
    }

    const sk_sp<Slide> fSlide;

    using INHERITED = sksg::RenderNode;
};

SkMatrix SlideMatrix(const sk_sp<Slide>& slide, const SkRect& dst) {
    const auto slideSize = slide->getDimensions();
    return SkMatrix::MakeRectToRect(SkRect::MakeIWH(slideSize.width(), slideSize.height()),
                                    dst,
                                    SkMatrix::kCenter_ScaleToFit);
}

} // namespace

struct SlideDir::Rec {
    sk_sp<Slide>           fSlide;
    sk_sp<sksg::Transform> fTransform;
    SkRect                 fRect;
};

class SlideDir::FocusController final : public sksg::Animator {
public:
    FocusController(const SlideDir* dir, const SkRect& focusRect)
        : fDir(dir)
        , fRect(focusRect)
        , fTarget(nullptr)
        , fState(State::kIdle) {}

    bool hasFocus() const { return fState == State::kFocused; }

    void startFocus(const Rec* target) {
        fTarget = target;
        fM0 = SlideMatrix(fTarget->fSlide, fTarget->fRect);
        fM1 = SlideMatrix(fTarget->fSlide, fRect);

        fTimeBase = 0;
        fState = State::kFocusing;
    }

    void startUnfocus() {
        SkASSERT(fTarget);

        SkTSwap(fM1, fM0);

        fTimeBase = 0;
        fState = State::kUnfocusing;
    }

    bool onMouse(SkScalar x, SkScalar y, sk_app::Window::InputState state, uint32_t modifiers) {
        SkASSERT(fTarget);

        // Map coords to slide space.
        const auto xform = SkMatrix::MakeRectToRect(fRect,
                                                    SkRect::MakeSize(fDir->fWinSize),
                                                    SkMatrix::kCenter_ScaleToFit);
        const auto pt = xform.mapXY(x, y);

        return fTarget->fSlide->onMouse(pt.x(), pt.y(), state, modifiers);
    }

    bool onChar(SkUnichar c) {
        SkASSERT(fTarget);

        return fTarget->fSlide->onChar(c);
    }

protected:
    void onTick(float t) {
        if (!this->isAnimating())
            return;

        if (!fTimeBase) {
            fTimeBase = t;
        }

        const auto rel_t = SkTPin((t - fTimeBase) / kFocusDuration, 0.0f, 1.0f);

        SkMatrix m;
        for (int i = 0; i < 9; ++i) {
            m[i] = fM0[i] + rel_t * (fM1[i] - fM0[i]);
        }

        SkASSERT(fTarget);
        fTarget->fTransform->getMatrix()->setMatrix(m);

        if (rel_t < 1)
            return;

        switch (fState) {
        case State::kFocusing:
            fState = State::kFocused;
            break;
        case State::kUnfocusing:
            fState  = State::kIdle;
            break;

        case State::kIdle:
        case State::kFocused:
            SkASSERT(false);
            break;
        }
    }

private:
    enum class State {
        kIdle,
        kFocusing,
        kUnfocusing,
        kFocused,
    };

    bool isAnimating() const { return fState == State::kFocusing || fState == State::kUnfocusing; }

    const SlideDir* fDir;
    const SkRect    fRect;
    const Rec*      fTarget;

    SkMatrix        fM0       = SkMatrix::I(),
                    fM1       = SkMatrix::I();
    float           fTimeBase = 0;
    State           fState    = State::kIdle;

    using INHERITED = sksg::Animator;
};

SlideDir::SlideDir(const SkString& name, SkTArray<sk_sp<Slide>, true>&& slides, int columns)
    : fSlides(std::move(slides))
    , fColumns(columns) {
    fName = name;
}

static sk_sp<sksg::RenderNode> MakeLabel(const SkString& txt,
                                         const SkPoint& pos,
                                         const SkMatrix& dstXform) {
    const auto size = kLabelSize / std::sqrt(dstXform.getScaleX() * dstXform.getScaleY());
    auto text = sksg::Text::Make(nullptr, txt);
    text->setFlags(SkPaint::kAntiAlias_Flag);
    text->setSize(size);
    text->setAlign(SkPaint::kCenter_Align);
    text->setPosition(pos + SkPoint::Make(0, size));

    return sksg::Draw::Make(std::move(text), sksg::Color::Make(SK_ColorBLACK));
}

void SlideDir::load(SkScalar winWidth, SkScalar winHeight) {
    // Build a global scene using transformed animation fragments:
    //
    // [Group(root)]
    //     [Transform]
    //         [Group]
    //             [AnimationWrapper]
    //             [Draw]
    //                 [Text]
    //                 [Color]
    //     [Transform]
    //         [Group]
    //             [AnimationWrapper]
    //             [Draw]
    //                 [Text]
    //                 [Color]
    //     ...
    //

    fWinSize = SkSize::Make(winWidth, winHeight);
    const auto  cellWidth =  winWidth / fColumns;
    fCellSize = SkSize::Make(cellWidth, cellWidth / kAspectRatio);

    sksg::AnimatorList sceneAnimators;
    fRoot = sksg::Group::Make();

    for (int i = 0; i < fSlides.count(); ++i) {
        const auto& slide     = fSlides[i];
        slide->load(winWidth, winHeight);

        const auto  slideSize = slide->getDimensions();
        const auto  cell      = SkRect::MakeXYWH(fCellSize.width()  * (i % fColumns),
                                                 fCellSize.height() * (i / fColumns),
                                                 fCellSize.width(),
                                                 fCellSize.height()),
                    slideRect = cell.makeInset(kPadding.width(), kPadding.height());

        auto slideMatrix = SlideMatrix(slide, slideRect);
        auto adapter     = sk_make_sp<SlideAdapter>(slide);
        auto slideGrp    = sksg::Group::Make();
        slideGrp->addChild(sksg::Draw::Make(sksg::Rect::Make(SkRect::MakeIWH(slideSize.width(),
                                                                             slideSize.height())),
                                            sksg::Color::Make(0xfff0f0f0)));
        slideGrp->addChild(adapter);
        slideGrp->addChild(MakeLabel(slide->getName(),
                                     SkPoint::Make(slideSize.width() / 2, slideSize.height()),
                                     slideMatrix));
        auto slideTransform = sksg::Transform::Make(std::move(slideGrp), slideMatrix);

        sceneAnimators.push_back(adapter->makeForwardingAnimator());

        fRoot->addChild(slideTransform);
        fRecs.push_back({ slide, slideTransform, slideRect });
    }

    fScene = sksg::Scene::Make(fRoot, std::move(sceneAnimators));

    const auto focusRect = SkRect::MakeSize(fWinSize).makeInset(kFocusInset.width(),
                                                                kFocusInset.height());
    fFocusController = skstd::make_unique<FocusController>(this, focusRect);
}

void SlideDir::unload() {
    for (const auto& slide : fSlides) {
        slide->unload();
    }

    fRecs.reset();
    fScene.reset();
    fFocusController.reset();
    fRoot.reset();
    fTimeBase = 0;
}

SkISize SlideDir::getDimensions() const {
    return  SkSize::Make(fWinSize.width(),
                         fCellSize.height() * (fSlides.count() / fColumns)).toCeil();
}

void SlideDir::draw(SkCanvas* canvas) {
    fScene->render(canvas);
}

bool SlideDir::animate(const SkAnimTimer& timer) {
    if (fTimeBase == 0) {
        // Reset the animation time.
        fTimeBase = timer.msec();
    }

    const auto t = timer.msec() - fTimeBase;
    fScene->animate(t);
    fFocusController->tick(t);

    return true;
}

bool SlideDir::onChar(SkUnichar c) {
    if (fFocusController->hasFocus()) {
        if (c == kUnfocusKey) {
            fFocusController->startUnfocus();
            return true;
        }
        return fFocusController->onChar(c);
    }

    return false;
}

bool SlideDir::onMouse(SkScalar x, SkScalar y, sk_app::Window::InputState state,
                       uint32_t modifiers) {
    if (state == sk_app::Window::kMove_InputState || modifiers)
        return false;

    if (fFocusController->hasFocus()) {
        return fFocusController->onMouse(x, y, state, modifiers);
    }

    const auto* cell = this->findCell(x, y);
    if (!cell)
        return false;

    static constexpr SkScalar kClickMoveTolerance = 4;

    switch (state) {
    case sk_app::Window::kDown_InputState:
        fTrackingCell = cell;
        fTrackingPos = SkPoint::Make(x, y);
        break;
    case sk_app::Window::kUp_InputState:
        if (cell == fTrackingCell &&
            SkPoint::Distance(fTrackingPos, SkPoint::Make(x, y)) < kClickMoveTolerance) {

            // Move the slide cell to front.
            fRoot->removeChild(cell->fTransform);
            fRoot->addChild(cell->fTransform);

            fFocusController->startFocus(cell);
        }
        break;
    default:
        break;
    }

    return false;
}

const SlideDir::Rec* SlideDir::findCell(float x, float y) const {
    // TODO: use SG hit testing instead of layout info?
    const auto size = this->getDimensions();
    if (x < 0 || y < 0 || x >= size.width() || y >= size.height()) {
        return nullptr;
    }

    const int col = static_cast<int>(x / fCellSize.width()),
              row = static_cast<int>(y / fCellSize.height()),
              idx = row * fColumns + col;

    return idx <= fRecs.count() ? &fRecs[idx] : nullptr;
}