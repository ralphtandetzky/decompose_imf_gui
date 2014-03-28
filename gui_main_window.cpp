#include "gui_main_window.h"
#include "ui_gui_main_window.h"

#include "decompose_imf_lib/calculations.h"
#include "decompose_imf_lib/optimization_task.h"

#include "cpp_utils/exception.h"
#include "cpp_utils/formula_parser.h"
#include "cpp_utils/math_constants.h"
#include "cpp_utils/std_make_unique.h"
#include "cpp_utils/user_parameter.h"

#include "qt_utils/event_filter.h"
#include "qt_utils/exception_handling.h"
#include "qt_utils/invoke_in_thread.h"
#include "qt_utils/loop_thread.h"
#include "qt_utils/serialize_props.h"

#include <array>
#include <complex>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

#include <QtGui>
#include <QMessageBox>
#include <QEvent>
#include <QFileDialog>

static char const * const InputDataNameFromSettings = "InputData";
static char const * const InputDataFileNameFromSettings = "InputDataFileName";


namespace gui {

struct MainWindow::Impl
{
    Impl( MainWindow * parent )
        : mainWindow(parent)
    {
    }

    ~Impl()
    {
    }

    // Creates functions to find initial approximations and
    // registers them in the gui.
    void setInitializers()
    {
        // create table of functions to find initial approximations
        initializers["Zero"] =
            []( const std::vector<double> & f )
            { return std::vector<std::complex<double>>(f.size()+1); };
        initializers["Interpolate zeros"] =
            &dimf::getInitialApproximationByInterpolatingZeros;

        // display these methods in the gui.
        for ( const auto & initializer : initializers )
            ui.initApproxMethComboBox->addItem(
                QString::fromStdString(initializer.first) );
    }

    void readInputData( std::istream & is )
    {
        readProperties( is, serializers );

        // open the file in the samples file line edit, if any.
        samples.clear();
        const auto samplesFileName = ui.samplesFileLineEdit->text();
        if ( !samplesFileName.isEmpty() )
            QU_HANDLE_ALL_EXCEPTIONS_FROM {
                mainWindow->readSamplesFile( samplesFileName );
            };
    }

    /////////////////////
    // Gui thread data //
    /////////////////////

    // parent object
    MainWindow * mainWindow;
    // Contains Qt user interface elements.
    Ui::MainWindow ui;
    // Objects which help to load the values in the gui input widgets
    // during construction and to store them during destruction.
    std::vector<std::unique_ptr<qu::PropertySerializer>> serializers;
    // A table of different methods which find an approximate solution
    // to the optimization problem. The user can select the method in
    // the gui.
    std::map<std::string,std::function<
        std::vector<std::complex<double>>(
            const std::vector<double> &)>> initializers;
    // A table of different preprocessing functions that can be applied
    // to a bunch of input samples.
    std::map<std::string,std::function<
        std::vector<double>(
            const std::vector<double> args,
            std::vector<double> samples)>> preprocessors;
    // A vector of samples read from a file.
    std::vector<double> samples;
    std::unique_ptr<QLabel> graphDisplay;

    dimf::OptimizationTask optimizationTask;
    qu::LoopThread worker;
};


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m( std::make_unique<Impl>(this) )
{
    m->ui.setupUi(this);

    m->graphDisplay = std::make_unique<QLabel>();
    qu::installEventFilter( this, [this]( QObject *, QEvent * event )
    {
        if ( event->type() == QEvent::Close )
            QApplication::quit();
        return false;
    });
    m->graphDisplay->setWindowFlags(
        m->graphDisplay->windowFlags() & ~Qt::WindowMaximizeButtonHint );

    m->setInitializers();

    // set up serializers
//    qu::createPropertySerializers( this->findChildren<QCheckBox*>(),
//                                   std::back_inserter( m->serializers ) );
    qu::createPropertySerializers( this->findChildren<QDoubleSpinBox*>(),
                                   std::back_inserter( m->serializers ) );
    qu::createPropertySerializers( this->findChildren<QComboBox*>(),
                                   std::back_inserter( m->serializers ) );
    qu::createPropertySerializers( this->findChildren<QSpinBox*>(),
                                   std::back_inserter( m->serializers ) );
    qu::createPropertySerializers( this->findChildren<QLineEdit*>(),
                                   std::back_inserter( m->serializers ) );
    qu::createPropertySerializers( this->findChildren<QPlainTextEdit*>(),
                                   std::back_inserter( m->serializers ) );
    qu::createPropertySerializers( this->findChildren<QTabWidget*>(),
                                   std::back_inserter( m->serializers ) );

    // load serialized input widget entries from a settings file.
    std::stringstream inputData(
                QSettings()
                .value( InputDataNameFromSettings )
                .toString()
                .toStdString() );

    m->readInputData( inputData );
}


MainWindow::~MainWindow()
{
    QU_HANDLE_ALL_EXCEPTIONS_FROM
    {
        // stop any running optimization thread
        cancel();

        // store current values from gui input widget entries.
        std::ostringstream ss;
        writeProperties( ss, m->serializers );
        QSettings().setValue( InputDataNameFromSettings,
                              QString::fromStdString(ss.str()) );
    };
}


void MainWindow::optimize()
{
    dimf::OptimizationParams params{};

    // read values from gui
    const auto functionString = m->ui.functionLineEdit->text().toStdString();
    const auto tab = m->ui.samplesTabWidget->currentWidget();
    const auto areSamplesFromExpression = ( tab == m->ui.fromExpressionTab );
    const auto areSamplesFromFile = ( tab == m->ui.fromFileTab );
    params.swarmSize      = m->ui.swarmSizeSpinBox->value();
    params.angleDevDegs   = m->ui.angleDevSpinBox ->value();
    params.amplitudeDev   = m->ui.amplDevSpinBox  ->value();
    params.crossOverProb  = m->ui.coSpinBox       ->value();
    params.diffWeight     = m->ui.dwSpinBox       ->value();
    params.initializer    = m->initializers.at(
                m->ui.initApproxMethComboBox->currentText().toStdString());
    params.nParams      = m->ui.nBaseFuncsSpinBox->value();
    params.initSigmaUnits = m->ui.initSigmaSpinBox ->value();
    params.initTauUnits   = m->ui.initTauSpinBox   ->value();
    params.nodeDevUnits   = m->ui.nodeDevSpinBox   ->value();
    params.sigmaDevUnits  = m->ui.sigmaDevSpinBox  ->value();
    params.tauDevUnits    = m->ui.tauDevSpinBox    ->value();
    params.preprocessing  = m->ui.preprocessingTextEdit
            ->toPlainText().toStdString();
    params.interprocessing = m->ui.interprocessingTextEdit
            ->toPlainText().toStdString();

    // calculate the target function from the expression
    if ( areSamplesFromExpression )
    {
        const auto xmin           = m->ui.xminSpinBox     ->value();
        const auto xmax           = m->ui.xmaxSpinBox     ->value();
        const auto nSamples       = m->ui.nSamplesSpinBox ->value();
        // build expression tree for target function
        const auto expression = std::make_shared<cu::ExpressionTree>();
        const auto nParsedChars = expression->parse( functionString );
        if ( nParsedChars < functionString.size() )
        {
            std::stringstream ss;
            ss << "Target function is not valid. "
                  "There seems to be an error here: \n";
            ss << std::string( begin(functionString),
                               begin(functionString)+nParsedChars );
            ss << std::endl << ">>\t";
            ss << std::string( begin(functionString)+nParsedChars,
                               end(functionString) );
            QMessageBox msgBox;

            msgBox.setText( QString::fromStdString(ss.str()) );
            msgBox.exec();
            return;
        }
        for ( auto i = 0; i < nSamples; ++i )
            params.samples.push_back( xmin + (xmax-xmin)*i/(nSamples-1) );
        params.samples = expression->evaluate( params.samples );
        params.xIntervalWidth = xmax -xmin;
    }
    else if ( areSamplesFromFile )
    {
        if ( m->samples.empty() )
            CU_THROW( "Cannot start search, because there were no samples "
                      "read from any file. Please select a file first." );
        params.samples = m->samples;
        params.xIntervalWidth = params.samples.size();
    }
    else
        CU_THROW( "No tab is open for selecting the samples." );

    params.receiveBestFit = [&](
            const std::vector<double> & bestParams
            , double cost
            , size_t nSamples
            , size_t nIter
            , const std::vector<double> & f )
    {
        using namespace cu;
        const auto v = dimf::getSamplesFromParams( bestParams, nSamples );

        // console output
        std::cout << nIter << ' ' << cost << ' ' << std::endl;
        //for ( const auto & elem : v )
        //    printf( "%5d;", int(std::round(100*elem)));
        //std::cout << std::endl;

        const auto psize = 600.;
        const auto xscale = psize*2/(v.size()-2);
        const auto yscale = 20;
        QImage image{(int)psize,(int)psize,QImage::Format_RGB32};
        {
            QPainter painter{&image};
            painter.fillRect(0,0,psize,psize,Qt::black);
            auto reals = QPolygonF{};
            auto imags = QPolygonF{};
            for ( auto i = size_t{0}; i < v.size(); i+=2 )
            {
                reals << QPointF( xscale*i/2, -yscale*v[i  ] );
                imags << QPointF( xscale*i/2, -yscale*std::remainder(v[i+1],2*pi) );
            }
            const auto imf = dimf::calculateImfFromPairsOfReals( v );
            auto fPoly   = QPolygonF{};
            auto imfPoly = QPolygonF{};
            for ( auto i = size_t{0}; i < f.size(); ++i )
            {
                fPoly   << QPointF( xscale*i, -yscale*f  [i] );
                imfPoly << QPointF( xscale*i, -yscale*imf[i] );
            }
            reals  .translate(0,psize/2);
            imags  .translate(0,psize/2);
            fPoly  .translate(0,psize/2);
            imfPoly.translate(0,psize/2);
            painter.setRenderHint( QPainter::Antialiasing );
            painter.setPen( Qt::darkGray );
            painter.drawLine( 0, psize/2, psize, psize/2);
            painter.drawLine( 0, psize/2-pi*yscale, psize, psize/2-pi*yscale);
            painter.drawLine( 0, psize/2+pi*yscale, psize, psize/2+pi*yscale);
            painter.setPen( Qt::green );
            painter.drawPolyline( reals );
            painter.setPen( Qt::magenta );
            painter.drawPolyline( imags );
            painter.setPen( Qt::white );
            painter.drawPolyline( fPoly );
            painter.setPen( Qt::yellow );
            painter.drawPolyline( imfPoly );
        }
        QMetaObject::invokeMethod( this, "setDisplay",
                                   Q_ARG(QImage,image));
    };
    m->graphDisplay->show();

    m->optimizationTask.restart( params );
}


void MainWindow::cancel()
{
    m->optimizationTask.cancel();
}


void MainWindow::calculateNextImf()
{
    m->optimizationTask.continueWithNextImf();
}


void MainWindow::openInputFile()
{
    const auto qFileName = QFileDialog::getOpenFileName(
                this, "Select An Input Data File",
                QSettings().value(InputDataFileNameFromSettings).toString() );
    if ( qFileName.isNull() ) // user cancelled?
        return;
    std::ifstream file( qFileName.toStdString() );
    if ( !file.good() )
        CU_THROW( "Could not open the selected file." );
    m->readInputData( file );
    QSettings().setValue( InputDataFileNameFromSettings, qFileName );
}


void MainWindow::saveInputs()
{
    const auto qFileName =
            QSettings()
            .value(InputDataFileNameFromSettings)
            .toString();
    if (qFileName.isNull() || qFileName.isEmpty() )
    {
        saveInputsAs();
        return;
    }
    std::ofstream file( qFileName.toStdString() );
    if ( !file.good() )
    {
        saveInputsAs();
        return;
    }
    writeProperties( file, m->serializers );
}


void MainWindow::saveInputsAs()
{
    const auto qFileName = QFileDialog::getSaveFileName(
                this, "Save Input Data File",
                QSettings().value(InputDataFileNameFromSettings).toString() );
    if ( qFileName.isNull() ) // user cancelled?
        return;
    const auto fileName = qFileName.toStdString();
    std::ofstream file( fileName );
    if ( !file.good() )
        CU_THROW( "Failed to open the file '" + fileName + "'." );
    writeProperties( file, m->serializers );
    QSettings().setValue( InputDataFileNameFromSettings, qFileName );
}


void MainWindow::selectSamplesFile()
try
{
    const auto qFileName = QFileDialog::getOpenFileName(
                this, "Select A Samples File",
                m->ui.samplesFileLineEdit->text() );
    if ( qFileName.isNull() ) // user cancelled?
        return;
    readSamplesFile( qFileName );
}
catch (...)
{
    CU_THROW( "Could not read samples file successfully." );
}


void MainWindow::readSamplesFile( const QString & qFileName )
{
    cancel();
    qu::invokeInThreadAsync( &m->worker, [=]()
    {
    QU_HANDLE_ALL_EXCEPTIONS_FROM
    {
        const auto fileName = qFileName.toStdString();
        std::ifstream file{ fileName };
        if ( !file )
            CU_THROW( "Could not open the file \"" + fileName + "\"." );
        auto vals = std::vector<double>( std::istream_iterator<double>(file),
                                         std::istream_iterator<double>() );
        if ( file.bad() )
            CU_THROW( "The file \"" + fileName +
                      "\" could not be read." );
        if ( vals.empty() )
            CU_THROW( "The file \"" + fileName +
                      "\" does not contain samples." );
        if ( !file.eof() )
            CU_THROW( "The end of the file \"" + fileName +
                      "\" has not been reached." );

        qu::invokeInGuiThreadAsync( [=]()
        {
            m->ui.samplesFileLineEdit->setText( qFileName );
            m->samples = std::move(vals);
            m->ui.fileInfoLabel->setText(
                QString("The number of samples is %1.")
                        .arg( m->samples.size() ));
        } );
    }; } );
}


void MainWindow::setDisplay( const QImage & image )
{
    m->graphDisplay->resize( image.size() );
    m->graphDisplay->setPixmap( QPixmap::fromImage(image) );
}

} // namespace gui
