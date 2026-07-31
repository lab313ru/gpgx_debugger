#pragma once
// Boundary between the IDA-side dock glue (ida_dock.cpp, includes IDA headers,
// NO Qt) and the Qt-side widget builder (ida_views.cpp, includes Qt, NO IDA
// headers). They must never share a translation unit: IDA's pro.h and Qt's
// qbytearrayalgorithms.h both define qstrlen/qsnprintf/... as unconditional
// inline functions and collide (a real clash since IDA 9.x moved to stock Qt6).
//
// Neither side includes the other's toolkit headers — only this plain header.

// Number of dock views and their titles (defined in ida_views.cpp).
constexpr int SMD_DGX_VIEW_COUNT = 11;
extern const char* const smd_dgx_view_titles[SMD_DGX_VIEW_COUNT];

// Frame sink target: forwards an emulator frame to the "Screen" view if it is
// currently open (no-op otherwise). Called from the EMULATION THREAD, so the
// Qt side keeps the widget pointer under a lock.
void smd_dgx_push_frame(const unsigned char* data, int w, int h, int pitch,
                        int vpX, int vpY, int vpW, int vpH);

// Qt side (ida_views.cpp) — called by the IDA side:
//   attach: wrap an IDA TWidget (passed as its QWidget* as void*) with a layout
//           and the view widget for index `idx`.
// Where the save-state manager keeps its files. Only the IDA side knows the
// database path, and only the Qt side owns the widget, so it crosses here.
void smd_dgx_set_states_dir(const char* path);

void smd_dgx_view_attach(int idx, void* twidget_as_qwidget);
//   detach_all: stop the refresh timer and drop widget pointers (on term).
void smd_dgx_view_detach_all();

// IDA side (ida_dock.cpp) — called by the plugmod:
void smd_dgx_register_views();
void smd_dgx_unregister_views();
