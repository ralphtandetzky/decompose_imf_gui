#pragma once

#include <QMainWindow>
#include <memory>

namespace gui {

class MainWindow : public QMainWindow
{
    Q_OBJECT
    
public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

private slots:
    void optimize();
    void cancel();
    void calculateNextImf();
    void openInputFile();
    void saveInputs();
    void saveInputsAs();
    void selectSamplesFile();
    void readSamplesFile(const QString & qFileName );
    void setDisplay( const QImage & image );

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};


} // namespace gui
