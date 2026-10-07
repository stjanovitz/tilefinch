#include "tilefinch/psp_reader_policy.h"

bool psp_reader_automatic_admits(const ReaderDocumentAnalysis *analysis)
{
    return analysis != NULL && analysis->high_confidence
        && analysis->kind != READER_PAGE_RAW;
}

void psp_reader_mark_engaged(PspUiState *ui, bool automatic)
{
    if (ui == NULL) return;
    ui->reader_mode = true;
    ui->basic_mode = false;
    ui->reader_automatic = automatic;
    if (automatic) {
        psp_ui_show_status(ui, PSP_READER_AUTOMATIC_NOTE,
                           PSP_READER_AUTOMATIC_NOTE_FRAMES);
    }
}

void psp_reader_mark_left(PspUiState *ui)
{
    if (ui == NULL) return;
    ui->reader_mode = false;
    ui->reader_automatic = false;
}

bool psp_reader_carries_to_navigation(const PspUiState *ui)
{
    return ui != NULL && ui->reader_mode && !ui->reader_automatic;
}
