using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

public class VelaVectorPad : Control {
    public bool Compass { get; set; }
    public double X { get; set; }
    public double Y { get; set; }
    public double Z { get; set; }
    public double Reference { get; set; }
    public event EventHandler VectorChanged;
    bool dragging;
    public VelaVectorPad() {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        BackColor = Color.White; Reference = 1; Size = new Size(232, 220);
        Cursor = Cursors.Hand;
    }
    void Change(MouseEventArgs e) {
        double cx = Width / 2.0, cy = Height / 2.0, radius = Math.Min(Width, Height) * .37;
        if (Compass) {
            double a = Math.Atan2(e.X - cx, cy - e.Y);
            double magnitude = Math.Sqrt(X * X + Y * Y);
            if (magnitude < 1e-6 || Double.IsInfinity(magnitude)) magnitude = Reference;
            X = Math.Cos(a) * magnitude; Y = Math.Sin(a) * magnitude;
        } else {
            double yaw = (e.X - cx) / radius * Math.PI;
            double pitch = (cy - e.Y) / radius * Math.PI / 2;
            double magnitude = Math.Sqrt(X*X + Y*Y + Z*Z);
            if (magnitude < 1e-6 || Double.IsInfinity(magnitude)) magnitude = Reference;
            X = magnitude * Math.Cos(pitch) * Math.Sin(yaw);
            Y = magnitude * Math.Sin(pitch);
            Z = magnitude * Math.Cos(pitch) * Math.Cos(yaw);
        }
        Invalidate(); if (VectorChanged != null) VectorChanged(this, EventArgs.Empty);
    }
    protected override void OnMouseDown(MouseEventArgs e) { base.OnMouseDown(e); if(e.Button==MouseButtons.Left) { dragging=true; Capture=true; Change(e); } }
    protected override void OnMouseMove(MouseEventArgs e) { base.OnMouseMove(e); if(dragging) Change(e); }
    protected override void OnMouseUp(MouseEventArgs e) { base.OnMouseUp(e); dragging=false; Capture=false; }
    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e); Graphics g=e.Graphics; g.SmoothingMode=SmoothingMode.AntiAlias;
        float cx=Width/2f, cy=Height/2f, r=Math.Min(Width,Height)*.37f;
        using(Pen p=new Pen(Color.FromArgb(220,226,235),2)) {
            g.DrawEllipse(p,cx-r,cy-r,2*r,2*r);
            g.DrawLine(p,cx-r,cy,cx+r,cy);g.DrawLine(p,cx,cy-r,cx,cy+r);
            if(!Compass) { g.DrawEllipse(p,cx-r,cy-r*.28f,2*r,r*.56f);g.DrawEllipse(p,cx-r*.28f,cy-r,r*.56f,2*r); }
        }
        double length=Math.Sqrt(X*X+Y*Y+Z*Z), heading=Math.Atan2(Y,X);
        float dx,dy;
        if(Compass) {
            for(int i=0;i<36;i++) {
                double a=i*Math.PI/18;
                using(Pen p=new Pen(i%9==0?Color.FromArgb(43,66,102):Color.Silver,2))
                    g.DrawLine(p,cx+(float)Math.Sin(a)*r,cy-(float)Math.Cos(a)*r,cx+(float)Math.Sin(a)*(r-7),cy-(float)Math.Cos(a)*(r-7));
            }
            using(Pen p=new Pen(Color.FromArgb(50,135,230),7)) {
                double degrees=(heading*180/Math.PI+360)%360;
                g.DrawArc(p,cx-r+9,cy-r+9,2*r-18,2*r-18,-90,(float)Math.Max(.1,degrees));
            }
            dx=(float)Math.Sin(heading)*r*.69f;dy=-(float)Math.Cos(heading)*r*.69f;
        } else {
            dx=length>0?(float)(X/length)*r*.8f:0;dy=length>0?-(float)(Y/length)*r*.8f:0;
            g.DrawString("X →   Y ↑   Z ⊙",Font,Brushes.DimGray,8,8);
        }
        if(Single.IsNaN(dx)||Single.IsInfinity(dx))dx=0;if(Single.IsNaN(dy)||Single.IsInfinity(dy))dy=0;
        using(Pen p=new Pen(Color.FromArgb(36,126,225),5)) { p.EndCap=LineCap.ArrowAnchor;g.DrawLine(p,cx,cy,cx+dx,cy+dy); }
        using(Brush b=new SolidBrush(Color.FromArgb(36,126,225)))g.FillEllipse(b,cx+dx-6,cy+dy-6,12,12);
        if(Compass) {
            string t=((heading*180/Math.PI+360)%360).ToString("0.0")+"°";
            SizeF size=g.MeasureString(t,Font);g.DrawString(t,Font,Brushes.Black,cx-size.Width/2,Height-24);
            g.DrawString("N",Font,Brushes.DarkRed,cx-6,cy-r-20);
        } else g.DrawString("拖动改变三轴方向",Font,Brushes.DimGray,18,Height-24);
    }
}

public class VelaIndicator : Control {
    public bool Active { get; set; }
    public Color ActiveColor { get; set; }
    public string Caption { get; set; }
    public VelaIndicator() {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer,true);
        ActiveColor=Color.LimeGreen;Size=new Size(180,62);Caption="";
    }
    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e);Graphics g=e.Graphics;g.SmoothingMode=SmoothingMode.AntiAlias;
        Color c=Active?ActiveColor:Color.FromArgb(180,185,194);
        using(Brush b=new SolidBrush(Color.FromArgb(50,c)))g.FillEllipse(b,2,8,42,42);
        using(Brush b=new SolidBrush(c))g.FillEllipse(b,10,16,26,26);
        g.DrawString(Caption,Font,Brushes.Black,50,10);
        g.DrawString(Active?"亮 / ON":"灭 / OFF",Font,Active?Brushes.DarkGreen:Brushes.Gray,50,33);
    }
}
