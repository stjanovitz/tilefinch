const box = (style = '', children = '') => '<div style="' + style + '">' + children + '</div>';
const flex = (style, ...children) =>
  box('display:flex;width:100px;height:60px;' + style, children.join(''));
const grid = (style, ...children) =>
  box('display:grid;width:120px;height:60px;' + style, children.join(''));

async function geometry(markup, axis, expected, nested = false) {
  const parent = item(markup);
  await new Promise(resolve => setTimeout(resolve, 15));
  const origin = parent.getBoundingClientRect();
  const results = [...(nested ? parent.firstElementChild : parent).children].map(child => {
    const rect = child.getBoundingClientRect();
    return axis === 'x' ? [rect.left - origin.left, rect.width]
                        : [rect.top - origin.top, rect.height];
  });
  return JSON.stringify(results) === JSON.stringify(expected);
}

const cases = {
  columnPercentUsesFullWidth() {
    return geometry(grid('grid-template-columns:50% 50%;column-gap:12px;', box(), box()),
      'x', [[0, 60], [72, 60]]);
  },
  rowPercentUsesFullHeight() {
    return geometry(grid('grid-template-rows:50% 50%;row-gap:12px;', box(), box()),
      'y', [[0, 30], [42, 30]]);
  },
  implicitPercentRows() {
    return geometry(grid('grid-auto-rows:25%;row-gap:3px;', box(), box(), box()),
      'y', [[0, 15], [18, 15], [36, 15]]);
  },
  smallFractionColumnsLeaveRestUnused() {
    return geometry(grid('grid-template-columns:.25fr .5fr;', box(), box()),
      'x', [[0, 30], [30, 60]]);
  },
  smallFractionRowsLeaveRestUnused() {
    return geometry(grid('grid-template-rows:.25fr .5fr;', box(), box()),
      'y', [[0, 15], [15, 30]]);
  },
  zeroFractionPreservesColumnFloor() {
    return geometry(grid('grid-template-columns:minmax(15px,0fr) 1fr;', box(), box()),
      'x', [[0, 15], [15, 105]]);
  },
  zeroFractionPreservesRowFloor() {
    return geometry(grid('grid-template-rows:minmax(15px,0fr) 1fr;', box(), box()),
      'y', [[0, 15], [15, 45]]);
  },
  freezeIntrinsicTrackBeforeRedistributing() {
    return geometry(grid('grid-template-columns:1fr 2fr;', box('', box('width:75px')), box()),
      'x', [[0, 75], [75, 45]]);
  },
  explicitZeroTrackMinimumDoesNotUseContentFloor() {
    return geometry(grid('grid-template-columns:minmax(0,1fr) minmax(0,2fr);',
      box('', box('width:75px')), box()), 'x', [[0, 40], [40, 80]]);
  },
  columnDenseFlowSearchesEarlierHoles() {
    return geometry(grid('grid-template-rows:repeat(3,10px);grid-auto-columns:30px;grid-auto-flow:column dense;',
      box('grid-row:span 2'), box('grid-row:span 2'), box()), 'x', [[0, 30], [30, 30], [0, 30]]);
  },
  columnSparseFlowMovesSpanToNextColumn() {
    return geometry(grid('grid-template-rows:repeat(3,10px);grid-auto-columns:30px;grid-auto-flow:column;',
      box('grid-row:span 2'), box('grid-row:span 2'), box()), 'x', [[0, 30], [30, 30], [30, 30]]);
  },
  autoFitCollapsesInteriorTracksWithoutRenumbering() {
    return geometry(grid('width:160px;grid-template-columns:repeat(auto-fit,minmax(30px,1fr));gap:10px;',
      box('grid-column:1'), box('grid-column:4')), 'x', [[0, 75], [85, 75]]);
  },
  autoFitKeepsTracksUnderASpan() {
    return geometry(grid('width:170px;grid-template-columns:repeat(auto-fit,minmax(30px,1fr));gap:10px;',
      box('grid-column:1 / 3'), box('grid-column:4')), 'x', [[0, 110], [120, 50]]);
  },
  subgridSpanClipsAtInheritedEnd() {
    return geometry(grid('grid-template-columns:30px 40px;',
      box('display:grid;grid-column:span 2;grid-template-columns:subgrid;', box('grid-column:2 / span 3'))),
      'x', [[30, 40]], true);
  },
  subgridSpanClipsAtInheritedStart() {
    return geometry(grid('grid-template-columns:30px 40px;',
      box('display:grid;grid-column:span 2;grid-template-columns:subgrid;', box('grid-column:-5 / 2'))),
      'x', [[0, 30]], true);
  },
  flexGrowthReassignsClampedSpace() {
    return geometry(flex('', box('flex:1 0 10px;max-width:20px'), box('flex:1 0 10px')),
      'x', [[0, 20], [20, 80]]);
  },
  flexShrinkReassignsClampedDeficit() {
    return geometry(flex('', box('flex:0 1 80px;min-width:70px'), box('flex:0 1 80px;min-width:0')),
      'x', [[0, 70], [70, 30]]);
  },
  columnGrowthReassignsClampedSpace() {
    return geometry(flex('flex-direction:column;', box('flex:1;height:10px;max-height:18px'),
      box('flex:1;height:10px')), 'y', [[0, 18], [18, 42]]);
  },
  columnShrinkReassignsClampedDeficit() {
    return geometry(flex('flex-direction:column;', box('flex:0 1 50px;min-height:38px'),
      box('flex:0 1 50px;min-height:0')), 'y', [[0, 38], [38, 22]]);
  },
  stretchTransfersAspectRatioToMainAxis() {
    return geometry(flex('height:40px;', box('aspect-ratio:2 / 1;flex:none'), box('flex:1')),
      'x', [[0, 80], [80, 20]]);
  },
  flexReverseStillHonorsLogicalStart() {
    return geometry(flex('flex-direction:row-reverse;justify-content:start;',
      box('width:35px'), box('width:35px')), 'x', [[35, 35], [0, 35]]);
  },
  flexReverseStillHonorsLogicalEnd() {
    return geometry(flex('flex-direction:row-reverse;justify-content:end;',
      box('width:35px'), box('width:35px')), 'x', [[65, 35], [30, 35]]);
  },
  flexRtlReverseStartsAtTheLeft() {
    return geometry(flex('direction:rtl;flex-direction:row-reverse;',
      box('width:35px'), box('width:35px')), 'x', [[0, 35], [35, 35]]);
  },
  flexColumnHonorsLogicalEnd() {
    return geometry(flex('flex-direction:column;justify-content:end;',
      box('height:15px'), box('height:15px')), 'y', [[30, 15], [45, 15]]);
  },
  flexReverseColumnHonorsLogicalStart() {
    return geometry(flex('flex-direction:column-reverse;justify-content:start;',
      box('height:15px'), box('height:15px')), 'y', [[15, 15], [0, 15]]);
  },
  flexUnsafeCenterKeepsNegativeOffset() {
    return geometry(flex('justify-content:unsafe center;',
      box('flex:none;width:120px')), 'x', [[-10, 120]]);
  },
  gridUnsafeCenterKeepsNegativeOffset() {
    return geometry(grid('grid-template-rows:80px;align-content:unsafe center;',
      box()), 'y', [[-10, 80]]);
  },
  gridLeftIsPhysicalInRtl() {
    return geometry(grid('direction:rtl;justify-content:left;grid-template-columns:20px 25px;',
      box(), box()), 'x', [[25, 20], [0, 25]]);
  },
  namedGridLineOccurrences() {
    return geometry(grid('grid-template-columns:[a] 20px [a] 30px [a] 40px [a];',
      box('grid-column:a 2 / 3 a'), box('grid-column:a -2 / -1 a')),
      'x', [[20, 30], [50, 40]]);
  },
  inheritedGridLineOccurrences() {
    return geometry(grid('grid-template-columns:[a] 20px [a] 30px [a] 40px [a];',
      box('display:grid;grid-column:span 3;grid-template-columns:subgrid;',
        box('grid-column:a -2 / a -1'))), 'x', [[50, 40]], true);
  },
  percentFloorClampsColumnFractionBeforeRedistributing() {
    return geometry(grid('grid-template-columns:minmax(50%,1fr) 2fr;column-gap:10px;',
      box(), box()), 'x', [[0, 60], [70, 50]]);
  },
  async percentFloorClampsRowFractionBeforeRedistributing() {
    return await geometry(grid('grid-template-rows:minmax(50%,1fr) 2fr;row-gap:10px;',
      box(), box()), 'y', [[0, 30], [40, 20]]) &&
      await geometry(grid('grid-template-rows:minmax(50%,20px) 20px;',
        box(), box()), 'y', [[0, 30], [30, 20]]);
  },
};
